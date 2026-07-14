/* SPDX-License-Identifier: GPL-2.0 */
/*
 * First migration milestone: exercise a child with its own CSpace/VSpace,
 * diagnose an intentional fault, destroy it, and start a replacement.  The
 * existing in-root LKL boot runs only after this boundary has been verified.
 */
#include "luna_isolation.h"
#include "luna_isolation_protocol.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/thread.h>
#include <vka/object.h>
#include <vka/capops.h>
#include <utils/util.h>
#include <stdio.h>
#include <string.h>

#define CHILD_PRIORITY 100
#define CHILD_BADGE_FAULT 0x41
#define CHILD_BADGE_CLEAN 0x42

/* Page-aligned manager ELF storage: present in the root VSpace, absent from the
 * separately loaded child ELF, and requires no extra Untyped allocation. */
static seL4_Word manager_private_page[BIT(seL4_PageBits) / sizeof(seL4_Word)]
    __attribute__((aligned(BIT(seL4_PageBits))));

struct luna_resource_slot {
    sel4utils_thread_t thread;
    vka_object_t join_ntfn;
    seL4_CPtr child_tcb;
    seL4_CPtr child_join_ntfn;
    int thread_configured;
    int join_allocated;
};

struct luna_child_resources {
    struct luna_resource_slot slots[LUNA_RESOURCE_SLOTS];
};

static void delete_child_cap(sel4utils_process_t *process, seL4_CPtr cap)
{
    if (!cap || !process->cspace.cptr) return;
    cspacepath_t path = {
        .root = process->cspace.cptr,
        .capPtr = cap,
        .capDepth = process->cspace_size,
    };
    int error = vka_cnode_delete(&path);
    if (error)
        printf("luna: failed to delete child resource cap %lu: %d\n", cap, error);
}

static void destroy_child(sel4utils_process_t *process,
                          struct luna_child_resources *resources, vka_t *vka)
{
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct luna_resource_slot *slot = &resources->slots[i];
        if (slot->thread_configured)
            seL4_TCB_Suspend(slot->thread.tcb.cptr);

        /* Delete foreign CSpace copies before returning their backing objects
         * to the Untyped allocator. Otherwise the allocator can offer a range
         * that the kernel still considers occupied by a derived cap. */
        delete_child_cap(process, slot->child_tcb);
        delete_child_cap(process, slot->child_join_ntfn);

        if (slot->thread_configured)
            sel4utils_clean_up_thread(vka, &process->vspace, &slot->thread);
        if (slot->join_allocated)
            vka_free_object(vka, &slot->join_ntfn);
    }
    if (process->cspace.cptr)
        sel4utils_destroy_process(process, vka);
    memset(resources, 0, sizeof(*resources));
    memset(process, 0, sizeof(*process));
}

static int configure_resource_pool(simple_t *simple, vka_t *vka,
                                   vspace_t *manager_vspace,
                                   sel4utils_process_t *process,
                                   struct luna_child_resources *resources)
{
    seL4_Word cspace_data =
        api_make_guard_skip_word(seL4_WordBits - process->cspace_size);

    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct luna_resource_slot *slot = &resources->slots[i];
        sel4utils_thread_config_t config = {0};
        config = thread_config_cspace(config, process->cspace.cptr, cspace_data);
        config = thread_config_fault_endpoint(config, SEL4UTILS_ENDPOINT_SLOT);
        config = thread_config_auth(config, simple_get_tcb(simple));
        config = thread_config_priority(config, CHILD_PRIORITY);
        config = thread_config_stack_size(config, LUNA_RESOURCE_STACK_PAGES);
        config = thread_config_create_reply(config);

        if (sel4utils_configure_thread_config(vka, manager_vspace,
                                              &process->vspace, config,
                                              &slot->thread)) {
            printf("luna: resource slot %d thread configure failed\n", i);
            return -1;
        }
        slot->thread_configured = 1;

        slot->child_tcb = sel4utils_copy_cap_to_process(
            process, vka, slot->thread.tcb.cptr);
        if (!slot->child_tcb) {
            printf("luna: resource slot %d TCB cap copy failed\n", i);
            return -1;
        }

        if (vka_alloc_notification(vka, &slot->join_ntfn)) {
            printf("luna: resource slot %d join allocation failed\n", i);
            return -1;
        }
        slot->join_allocated = 1;
        slot->child_join_ntfn = sel4utils_copy_cap_to_process(
            process, vka, slot->join_ntfn.cptr);
        if (!slot->child_join_ntfn) {
            printf("luna: resource slot %d join cap copy failed\n", i);
            return -1;
        }
    }
    return 0;
}

static int receive_event(seL4_CPtr control_ep, seL4_Word expected_badge,
                         seL4_Word expected_event, seL4_Word expected_detail)
{
    seL4_Word badge = 0;
    seL4_MessageInfo_t tag = seL4_Recv(control_ep, &badge);
    seL4_Word label = seL4_MessageInfo_get_label(tag);
    seL4_Word length = seL4_MessageInfo_get_length(tag);
    seL4_Word event = length > 0 ? seL4_GetMR(0) : 0;
    seL4_Word detail = length > 1 ? seL4_GetMR(1) : 0;

    if (label != 0 || length < 2 || badge != expected_badge ||
        event != expected_event || detail != expected_detail) {
        printf("luna: isolation event mismatch label=%lu len=%lu badge=%lu "
               "event=%lu detail=%lu\n",
               label, length, badge, event, detail);
        return -1;
    }
    return 0;
}

static int start_child(simple_t *simple, vka_t *vka, vspace_t *manager_vspace,
                       seL4_CPtr control_ep, seL4_CPtr command_ntfn,
                       seL4_Word badge,
                       enum luna_isolation_mode mode, seL4_Word private_addr,
                       sel4utils_process_t *process,
                       struct luna_child_resources *resources)
{
    sel4utils_process_config_t config =
        process_config_default_simple(simple, LUNA_ISOLATION_CHILD_IMAGE, CHILD_PRIORITY);
    if (sel4utils_configure_process_custom(process, vka, manager_vspace, config)) {
        printf("luna: isolation child configure failed\n");
        return -1;
    }

    if (!process->own_cspace || !process->own_vspace || !process->own_ep) {
        printf("luna: isolation child did not receive independent resources\n");
        destroy_child(process, resources, vka);
        return -1;
    }
    if (process->pd.cptr == simple_get_pd(simple) ||
        process->cspace.cptr == simple_get_cnode(simple)) {
        printf("luna: isolation child reused manager address or capability space\n");
        destroy_child(process, resources, vka);
        return -1;
    }

    if (configure_resource_pool(simple, vka, manager_vspace, process, resources)) {
        destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t control_path;
    vka_cspace_make_path(vka, control_ep, &control_path);
    seL4_CPtr child_control = sel4utils_mint_cap_to_process(
        process, control_path, seL4_CapRights_new(false, false, false, true), badge);
    if (!child_control) {
        printf("luna: isolation control cap mint failed\n");
        destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t command_path;
    vka_cspace_make_path(vka, command_ntfn, &command_path);
    seL4_CPtr child_command = sel4utils_mint_cap_to_process(
        process, command_path, seL4_CapRights_new(false, false, true, false), 0);
    if (!child_command) {
        printf("luna: isolation command cap mint failed\n");
        destroy_child(process, resources, vka);
        return -1;
    }

    struct luna_resource_slot *slot = &resources->slots[0];
    char arg_strings[10][WORD_STRING_SIZE];
    char *argv[10];
    sel4utils_create_word_args(arg_strings, argv, 10,
                               (seL4_Word)child_control,
                               (seL4_Word)child_command,
                               (seL4_Word)mode,
                               private_addr,
                               (seL4_Word)LUNA_RESOURCE_SLOTS,
                               (seL4_Word)slot->child_tcb,
                               (seL4_Word)(uintptr_t)slot->thread.stack_top,
                               (seL4_Word)slot->thread.stack_size,
                               slot->thread.ipc_buffer_addr,
                               (seL4_Word)slot->child_join_ntfn);
    if (sel4utils_spawn_process_v(process, vka, manager_vspace, 10, argv, 1)) {
        printf("luna: isolation child spawn failed\n");
        destroy_child(process, resources, vka);
        return -1;
    }
    return 0;
}

int luna_isolation_smoke(simple_t *simple, vka_t *vka, vspace_t *manager_vspace)
{
    const seL4_Word private_addr = (seL4_Word)(uintptr_t)manager_private_page;
    vka_object_t control_ep = {0};
    vka_object_t command_ntfn = {0};
    sel4utils_process_t child = {0};
    struct luna_child_resources resources = {0};
    int control_allocated = 0;
    int command_allocated = 0;
    int child_live = 0;
    int result = -1;

    manager_private_page[0] = LUNA_ISOLATION_SECRET;

    if (vka_alloc_endpoint(vka, &control_ep)) {
        printf("luna: isolation control endpoint allocation failed\n");
        goto out;
    }
    control_allocated = 1;

    if (vka_alloc_notification(vka, &command_ntfn)) {
        printf("luna: isolation command notification allocation failed\n");
        goto out;
    }
    command_allocated = 1;

    if (start_child(simple, vka, manager_vspace, control_ep.cptr, command_ntfn.cptr,
                    CHILD_BADGE_FAULT, LUNA_ISOLATION_MODE_FAULT,
                    private_addr, &child, &resources))
        goto out;
    child_live = 1;
    printf("luna: isolation child created with private CSpace/VSpace\n");

    if (receive_event(control_ep.cptr, CHILD_BADGE_FAULT,
                      LUNA_ISOLATION_EVENT_READY, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    if (receive_event(control_ep.cptr, CHILD_BADGE_FAULT,
                      LUNA_ISOLATION_EVENT_LKL_LINKED, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    printf("LUNA_LKL_CHILD_LINKED\n");
    seL4_Signal(command_ntfn.cptr);

    if (receive_event(control_ep.cptr, CHILD_BADGE_FAULT,
                      LUNA_ISOLATION_EVENT_RESOURCE_OK, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    printf("LUNA_RESOURCE_POOL_OK\n");
    if (receive_event(control_ep.cptr, CHILD_BADGE_FAULT,
                      LUNA_ISOLATION_EVENT_LKL_INIT_OK, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    printf("LUNA_LKL_CHILD_INIT_OK\n");

    seL4_Word fault_badge = 0;
    seL4_MessageInfo_t fault_tag = seL4_Recv(child.fault_endpoint.cptr, &fault_badge);
    seL4_Fault_t fault = seL4_getFault(fault_tag);
    if (seL4_Fault_get_seL4_FaultType(fault) != seL4_Fault_VMFault ||
        seL4_Fault_VMFault_get_Addr(fault) != private_addr) {
        printf("luna: unexpected isolation fault label=%lu addr=%p\n",
               seL4_MessageInfo_get_label(fault_tag),
               (void *)(uintptr_t)seL4_Fault_VMFault_get_Addr(fault));
        goto out;
    }
    sel4utils_print_fault_message(fault_tag, LUNA_ISOLATION_CHILD_IMAGE);
    if (*(volatile seL4_Word *)(uintptr_t)private_addr != LUNA_ISOLATION_SECRET) {
        printf("luna: manager-private page was modified\n");
        goto out;
    }
    printf("LUNA_ISOLATION_FAULT_OK addr=%p\n", (void *)(uintptr_t)private_addr);

    destroy_child(&child, &resources, vka);
    child_live = 0;

    if (start_child(simple, vka, manager_vspace, control_ep.cptr, command_ntfn.cptr,
                    CHILD_BADGE_CLEAN, LUNA_ISOLATION_MODE_CLEAN,
                    private_addr, &child, &resources))
        goto out;
    child_live = 1;

    if (receive_event(control_ep.cptr, CHILD_BADGE_CLEAN,
                      LUNA_ISOLATION_EVENT_READY, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (receive_event(control_ep.cptr, CHILD_BADGE_CLEAN,
                      LUNA_ISOLATION_EVENT_LKL_LINKED, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    seL4_Signal(command_ntfn.cptr);
    if (receive_event(control_ep.cptr, CHILD_BADGE_CLEAN,
                      LUNA_ISOLATION_EVENT_RESOURCE_OK, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (receive_event(control_ep.cptr, CHILD_BADGE_CLEAN,
                      LUNA_ISOLATION_EVENT_LKL_INIT_OK, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (receive_event(control_ep.cptr, CHILD_BADGE_CLEAN,
                      LUNA_ISOLATION_EVENT_DONE, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    printf("LUNA_ISOLATION_CHANNEL_OK\n");
    printf("LUNA_ISOLATION_RESTART_OK\n");
    destroy_child(&child, &resources, vka);
    child_live = 0;
    result = 0;

out:
    if (child_live) destroy_child(&child, &resources, vka);
    if (command_allocated) vka_free_object(vka, &command_ntfn);
    if (control_allocated) vka_free_object(vka, &control_ep);
    return result;
}
