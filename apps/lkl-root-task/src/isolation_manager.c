/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Own an LKL child with its own CSpace/VSpace, diagnose an intentional fault,
 * destroy it, and start a clean replacement without exposing manager
 * allocation authority to the child.
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
#define CHILD_BADGE_STRESS 0x43
#define MANAGER_PRIVATE_ADDR ((void *)(uintptr_t)0x40000000UL)

struct luna_resource_slot {
    sel4utils_thread_t thread;
    vka_object_t join_ntfn;
    seL4_CPtr child_tcb;
    seL4_CPtr child_join_ntfn;
    int thread_configured;
    int join_allocated;
};

struct luna_sync_slot {
    vka_object_t ntfn;
    seL4_CPtr child_ntfn;
    int allocated;
};

struct luna_console_resource {
    cspacepath_t io_port_path;
    seL4_CPtr child_io_port;
    int slot_allocated;
    int cap_created;
};

struct luna_heap_resource {
    reservation_t reservation;
    unsigned char mapped[LUNA_CHILD_HEAP_PAGES];
    size_t mapped_pages;
    size_t peak_pages;
    int reserved;
};

struct luna_child_resources {
    struct luna_resource_slot slots[LUNA_RESOURCE_SLOTS];
    struct luna_sync_slot sync[LUNA_SYNC_SLOTS];
    struct luna_console_resource console;
    struct luna_heap_resource heap;
};

struct luna_event_context {
    seL4_CPtr control_ep;
    seL4_CPtr command_ep;
    seL4_Word badge;
    sel4utils_process_t *process;
    struct luna_child_resources *resources;
    vka_t *vka;
};

static int delete_child_cap(sel4utils_process_t *process, seL4_CPtr cap)
{
    if (!cap || !process->cspace.cptr) return 0;
    cspacepath_t path = {
        .root = process->cspace.cptr,
        .capPtr = cap,
        .capDepth = process->cspace_size,
    };
    int error = vka_cnode_delete(&path);
    if (error)
        printf("luna: failed to delete child resource cap %lu: %d\n", cap, error);
    return error;
}

static int destroy_child(sel4utils_process_t *process,
                         struct luna_child_resources *resources, vka_t *vka)
{
    int result = 0;
    if (process->thread.tcb.cptr)
        seL4_TCB_Suspend(process->thread.tcb.cptr);
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct luna_resource_slot *slot = &resources->slots[i];
        if (slot->thread_configured)
            seL4_TCB_Suspend(slot->thread.tcb.cptr);

        /* Delete foreign CSpace copies before returning their backing objects
         * to the Untyped allocator. Otherwise the allocator can offer a range
         * that the kernel still considers occupied by a derived cap. */
        if (delete_child_cap(process, slot->child_tcb)) result = -1;
        if (delete_child_cap(process, slot->child_join_ntfn)) result = -1;

        if (slot->thread_configured)
            sel4utils_clean_up_thread(vka, &process->vspace, &slot->thread);
        if (slot->join_allocated)
            vka_free_object(vka, &slot->join_ntfn);
    }
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        struct luna_sync_slot *slot = &resources->sync[i];
        if (delete_child_cap(process, slot->child_ntfn)) result = -1;
        if (slot->allocated)
            vka_free_object(vka, &slot->ntfn);
    }
    if (delete_child_cap(process, resources->console.child_io_port)) result = -1;
    if (resources->console.cap_created &&
        vka_cnode_delete(&resources->console.io_port_path))
        result = -1;
    if (resources->console.slot_allocated)
        vka_cspace_free_path(vka, resources->console.io_port_path);
    if (resources->heap.reserved) {
        for (size_t i = 0; i < LUNA_CHILD_HEAP_PAGES;) {
            if (!resources->heap.mapped[i]) {
                i++;
                continue;
            }
            size_t start = i;
            while (i < LUNA_CHILD_HEAP_PAGES &&
                   resources->heap.mapped[i])
                i++;
            vspace_unmap_pages(
                &process->vspace,
                (void *)(uintptr_t)(LUNA_CHILD_HEAP_BASE +
                                    start * BIT(seL4_PageBits)),
                i - start, seL4_PageBits, vka);
        }
        vspace_free_reservation(&process->vspace,
                                resources->heap.reservation);
    }
    if (process->cspace.cptr)
        sel4utils_destroy_process(process, vka);
    memset(resources, 0, sizeof(*resources));
    memset(process, 0, sizeof(*process));
    return result;
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
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        struct luna_sync_slot *slot = &resources->sync[i];
        if (vka_alloc_notification(vka, &slot->ntfn)) {
            printf("luna: sync slot %d allocation failed\n", i);
            return -1;
        }
        slot->allocated = 1;
        slot->child_ntfn = sel4utils_copy_cap_to_process(
            process, vka, slot->ntfn.cptr);
        if (!slot->child_ntfn) {
            printf("luna: sync slot %d cap copy failed\n", i);
            return -1;
        }
    }
    struct luna_console_resource *console = &resources->console;
    if (vka_cspace_alloc_path(vka, &console->io_port_path)) {
        printf("luna: console I/O cap slot allocation failed\n");
        return -1;
    }
    console->slot_allocated = 1;
    if (simple_get_IOPort_cap(simple, 0x3f8, 0x3ff,
                              console->io_port_path.root,
                              console->io_port_path.capPtr,
                              console->io_port_path.capDepth)) {
        printf("luna: COM1 I/O cap allocation failed\n");
        return -1;
    }
    console->cap_created = 1;
    console->child_io_port = sel4utils_copy_cap_to_process(
        process, vka, console->io_port_path.capPtr);
    if (!console->child_io_port) {
        printf("luna: COM1 I/O cap copy failed\n");
        return -1;
    }
    return 0;
}

static int service_memory_request(struct luna_event_context *context,
                                  seL4_Word event, seL4_Word address,
                                  seL4_Word pages)
{
    struct luna_heap_resource *heap = &context->resources->heap;
    int error = 0;
    uintptr_t base = LUNA_CHILD_HEAP_BASE;
    uintptr_t end = base + LUNA_CHILD_HEAP_SIZE;
    uintptr_t request = (uintptr_t)address;
    size_t page_count = (size_t)pages;
    if (!heap->reserved || !page_count || request < base || request >= end ||
        (request & (BIT(seL4_PageBits) - 1)) ||
        page_count > (end - request) / BIT(seL4_PageBits)) {
        error = -1;
    }

    size_t first_page = error ? 0 :
        (request - base) / BIT(seL4_PageBits);
    if (!error) {
        for (size_t i = 0; i < page_count; i++) {
            int mapped = heap->mapped[first_page + i] != 0;
            if ((event == LUNA_ISOLATION_EVENT_MEMORY_MAP && mapped) ||
                (event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP && !mapped)) {
                error = -1;
                break;
            }
        }
    }

    if (!error && event == LUNA_ISOLATION_EVENT_MEMORY_MAP) {
        error = vspace_new_pages_at_vaddr(
            &context->process->vspace, (void *)request, page_count,
            seL4_PageBits, heap->reservation);
        if (!error) {
            memset(&heap->mapped[first_page], 1, page_count);
            heap->mapped_pages += page_count;
            if (heap->mapped_pages > heap->peak_pages)
                heap->peak_pages = heap->mapped_pages;
        }
    } else if (!error && event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP) {
        vspace_unmap_pages(&context->process->vspace, (void *)request,
                           page_count, seL4_PageBits, context->vka);
        memset(&heap->mapped[first_page], 0, page_count);
        heap->mapped_pages -= page_count;
    } else if (!error) {
        error = -1;
    }

    seL4_SetMR(0, LUNA_COMMAND_MEMORY_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_Send(context->command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    if (error)
        printf("luna: child heap request failed event=%lu addr=%p pages=%lu\n",
               event, (void *)request, pages);
    return error;
}

static int receive_event(struct luna_event_context *context,
                         seL4_Word expected_event,
                         seL4_Word expected_detail)
{
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t tag = seL4_Recv(context->control_ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(tag);
        seL4_Word length = seL4_MessageInfo_get_length(tag);
        seL4_Word event = length > 0 ? seL4_GetMR(0) : 0;
        seL4_Word detail = length > 1 ? seL4_GetMR(1) : 0;

        if (label == 0 && length == 3 && badge == context->badge &&
            (event == LUNA_ISOLATION_EVENT_MEMORY_MAP ||
             event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP)) {
            if (service_memory_request(context, event, seL4_GetMR(1),
                                       seL4_GetMR(2)))
                return -1;
            continue;
        }

        if (label != 0 || length < 2 || badge != context->badge ||
            event != expected_event || detail != expected_detail) {
            printf("luna: isolation event mismatch label=%lu len=%lu badge=%lu "
                   "event=%lu detail=%lu\n",
                   label, length, badge, event, detail);
            return -1;
        }
        if ((event == LUNA_ISOLATION_EVENT_ALLOCATOR_OK ||
             event == LUNA_ISOLATION_EVENT_ALLOCATOR_RELEASED) &&
            context->resources->heap.mapped_pages != 0) {
            printf("luna: child heap retained %zu pages at event %lu\n",
                   context->resources->heap.mapped_pages, event);
            return -1;
        }
        if (event == LUNA_ISOLATION_EVENT_ALLOCATOR_OK &&
            context->resources->heap.peak_pages !=
                LUNA_CHILD_HEAP_PAGES) {
            printf("luna: child heap peak was %zu pages, expected %lu\n",
                   context->resources->heap.peak_pages,
                   (unsigned long)LUNA_CHILD_HEAP_PAGES);
            return -1;
        }
        return 0;
    }
}

static void send_resource_slot(seL4_CPtr command_ep,
                               const struct luna_resource_slot *slot,
                               seL4_Word index)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_SLOT);
    seL4_SetMR(1, index);
    seL4_SetMR(2, LUNA_RESOURCE_SLOTS);
    seL4_SetMR(3, slot->child_tcb);
    seL4_SetMR(4, (seL4_Word)(uintptr_t)slot->thread.stack_top);
    seL4_SetMR(5, slot->thread.stack_size);
    seL4_SetMR(6, slot->thread.ipc_buffer_addr);
    seL4_SetMR(7, slot->child_join_ntfn);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 8));
}

static void send_start(seL4_CPtr command_ep, unsigned long long tsc_frequency)
{
    seL4_SetMR(0, LUNA_COMMAND_START);
    seL4_SetMR(1, (seL4_Word)tsc_frequency);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void send_sync_slot(seL4_CPtr command_ep,
                           const struct luna_sync_slot *slot,
                           seL4_Word index)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_SYNC);
    seL4_SetMR(1, index);
    seL4_SetMR(2, LUNA_SYNC_SLOTS);
    seL4_SetMR(3, slot->child_ntfn);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 4));
}

static void send_console_resource(
    seL4_CPtr command_ep, const struct luna_console_resource *console)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_CONSOLE);
    seL4_SetMR(1, console->child_io_port);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static int start_child(simple_t *simple, vka_t *vka, vspace_t *manager_vspace,
                       seL4_CPtr control_ep, seL4_CPtr command_ep,
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
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    if (process->pd.cptr == simple_get_pd(simple) ||
        process->cspace.cptr == simple_get_cnode(simple)) {
        printf("luna: isolation child reused manager address or capability space\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    resources->heap.reservation = vspace_reserve_range_at(
        &process->vspace, (void *)(uintptr_t)LUNA_CHILD_HEAP_BASE,
        LUNA_CHILD_HEAP_SIZE, seL4_AllRights, 1);
    if (!resources->heap.reservation.res) {
        printf("luna: child heap reservation failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    resources->heap.reserved = 1;

    if (configure_resource_pool(simple, vka, manager_vspace, process, resources)) {
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t control_path;
    vka_cspace_make_path(vka, control_ep, &control_path);
    seL4_CPtr child_control = sel4utils_mint_cap_to_process(
        process, control_path, seL4_CapRights_new(false, false, false, true), badge);
    if (!child_control) {
        printf("luna: isolation control cap mint failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t command_path;
    vka_cspace_make_path(vka, command_ep, &command_path);
    seL4_CPtr child_command = sel4utils_mint_cap_to_process(
        process, command_path, seL4_CapRights_new(false, false, true, false), 0);
    if (!child_command) {
        printf("luna: isolation command cap mint failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    char arg_strings[4][WORD_STRING_SIZE];
    char *argv[4];
    sel4utils_create_word_args(arg_strings, argv, 4,
                               (seL4_Word)child_control,
                               (seL4_Word)child_command,
                               (seL4_Word)mode,
                               private_addr);
    if (sel4utils_spawn_process_v(process, vka, manager_vspace, 4, argv, 1)) {
        printf("luna: isolation child spawn failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    return 0;
}

static int boot_child(simple_t *simple, vka_t *vka,
                      vspace_t *manager_vspace, seL4_CPtr control_ep,
                      seL4_CPtr command_ep, seL4_Word badge,
                      enum luna_isolation_mode mode, seL4_Word private_addr,
                      unsigned long long tsc_frequency,
                      sel4utils_process_t *process,
                      struct luna_child_resources *resources,
                      int *child_live,
                      struct luna_event_context *event_context)
{
    if (start_child(simple, vka, manager_vspace, control_ep, command_ep,
                    badge, mode, private_addr, process, resources))
        return -1;
    *child_live = 1;
    *event_context = (struct luna_event_context) {
        .control_ep = control_ep,
        .command_ep = command_ep,
        .badge = badge,
        .process = process,
        .resources = resources,
        .vka = vka,
    };

    if (receive_event(event_context, LUNA_ISOLATION_EVENT_READY, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_LINKED, mode))
        return -1;

    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++)
        send_resource_slot(command_ep, &resources->slots[i], i);
    for (seL4_Word i = 0; i < LUNA_SYNC_SLOTS; i++)
        send_sync_slot(command_ep, &resources->sync[i], i);
    send_console_resource(command_ep, &resources->console);
    if (receive_event(event_context,
                      LUNA_ISOLATION_EVENT_RESOURCE_CONFIGURED, mode))
        return -1;
    send_start(command_ep, tsc_frequency);

    if (receive_event(event_context, LUNA_ISOLATION_EVENT_RESOURCE_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_ALLOCATOR_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_INIT_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_BOOT_OK, mode))
        return -1;
    return 0;
}

static int wait_child_halt(struct luna_event_context *event_context,
                           enum luna_isolation_mode mode)
{
    if (receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_HALT_OK, mode) ||
        receive_event(event_context,
                      LUNA_ISOLATION_EVENT_ALLOCATOR_RELEASED, mode))
        return -1;
    return 0;
}

static int release_child(sel4utils_process_t *child,
                         struct luna_child_resources *resources,
                         vka_t *vka, int *child_live)
{
    int error = destroy_child(child, resources, vka);
    *child_live = 0;
    return error;
}

int luna_isolation_smoke(simple_t *simple, vka_t *vka,
                         vspace_t *manager_vspace,
                         unsigned long long tsc_frequency)
{
    reservation_t private_reservation = vspace_reserve_range_at(
        manager_vspace, MANAGER_PRIVATE_ADDR, BIT(seL4_PageBits),
        seL4_AllRights, 1);
    if (!private_reservation.res) {
        printf("luna: manager-private reservation failed\n");
        return -1;
    }
    if (vspace_new_pages_at_vaddr(manager_vspace, MANAGER_PRIVATE_ADDR, 1,
                                  seL4_PageBits, private_reservation)) {
        printf("luna: manager-private page allocation failed\n");
        vspace_free_reservation(manager_vspace, private_reservation);
        return -1;
    }
    volatile seL4_Word *manager_private_page = MANAGER_PRIVATE_ADDR;
    const seL4_Word private_addr =
        (seL4_Word)(uintptr_t)manager_private_page;
    vka_object_t control_ep = {0};
    vka_object_t command_ep = {0};
    sel4utils_process_t child = {0};
    struct luna_child_resources resources = {0};
    struct luna_event_context event_context = {0};
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

    if (vka_alloc_endpoint(vka, &command_ep)) {
        printf("luna: isolation command endpoint allocation failed\n");
        goto out;
    }
    command_allocated = 1;

    if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                   command_ep.cptr, CHILD_BADGE_FAULT,
                   LUNA_ISOLATION_MODE_FAULT, private_addr, tsc_frequency,
                   &child, &resources, &child_live, &event_context))
        goto out;
    printf("luna: isolation child created with private CSpace/VSpace\n");
    printf("LUNA_LKL_CHILD_LINKED\n");
    printf("LUNA_RESOURCE_POOL_OK\n");
    printf("LUNA_CHILD_ALLOCATOR_OK pages=%lu\n",
           (unsigned long)LUNA_CHILD_HEAP_PAGES);
    printf("LUNA_LKL_CHILD_INIT_OK\n");
    printf("LUNA_LKL_CHILD_BOOT_OK\n");
    if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    printf("LUNA_LKL_CHILD_HALT_OK\n");
    printf("LUNA_CHILD_ALLOCATOR_RELEASE_OK\n");

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
    if (manager_private_page[0] != LUNA_ISOLATION_SECRET) {
        printf("luna: manager-private page was modified\n");
        goto out;
    }
    printf("LUNA_ISOLATION_FAULT_OK addr=%p\n", (void *)(uintptr_t)private_addr);

    if (release_child(&child, &resources, vka, &child_live)) goto out;

    for (int round = 1; round <= LUNA_RESTART_STRESS_ROUNDS; round++) {
        if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                       command_ep.cptr, CHILD_BADGE_STRESS,
                       LUNA_ISOLATION_MODE_STRESS, private_addr,
                       tsc_frequency, &child, &resources, &child_live,
                       &event_context))
            goto out;
        if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_STRESS) ||
            receive_event(&event_context, LUNA_ISOLATION_EVENT_DONE,
                          LUNA_ISOLATION_MODE_STRESS))
            goto out;
        if (release_child(&child, &resources, vka, &child_live)) goto out;
        if (round % 10 == 0)
            printf("luna: restart stress %d/%d\n", round,
                   LUNA_RESTART_STRESS_ROUNDS);
    }
    printf("LUNA_RESTART_STRESS_OK rounds=%d\n",
           LUNA_RESTART_STRESS_ROUNDS);

    if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                   command_ep.cptr, CHILD_BADGE_CLEAN,
                   LUNA_ISOLATION_MODE_CLEAN, private_addr, tsc_frequency,
                   &child, &resources, &child_live, &event_context))
        goto out;
    if (receive_event(&event_context, LUNA_ISOLATION_EVENT_LKL_SHELL_READY,
                      LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    printf("LUNA_LKL_CHILD_SHELL_READY\n");
    if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (receive_event(&event_context, LUNA_ISOLATION_EVENT_DONE,
                      LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    printf("LUNA_ISOLATION_CHANNEL_OK\n");
    printf("LUNA_ISOLATION_RESTART_OK\n");
    if (release_child(&child, &resources, vka, &child_live)) goto out;
    result = 0;

out:
    if (child_live) (void)destroy_child(&child, &resources, vka);
    if (command_allocated) vka_free_object(vka, &command_ep);
    if (control_allocated) vka_free_object(vka, &control_ep);
    vspace_unmap_pages(manager_vspace, MANAGER_PRIVATE_ADDR, 1,
                       seL4_PageBits, vka);
    vspace_free_reservation(manager_vspace, private_reservation);
    return result;
}
