/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal payload used to establish the process boundary that LKL will move
 * into. It receives a send-only event endpoint, a receive-only command
 * endpoint, plus the capabilities installed by sel4utils for its own
 * CSpace, VSpace and TCB.
 */
#include "luna_isolation_protocol.h"
#include "luna_lkl_task_host.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <lkl_host.h>
#include <string.h>

static seL4_Word parse_word(const char *s)
{
    seL4_Word value = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (seL4_Word)(*s - '0');
        s++;
    }
    return value;
}

static void send_event(seL4_CPtr control_ep, seL4_Word event, seL4_Word detail)
{
    seL4_SetMR(0, event);
    seL4_SetMR(1, detail);
    seL4_Send(control_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static seL4_MessageInfo_t receive_command(seL4_CPtr command_ep,
                                          enum luna_manager_command expected)
{
    seL4_Word badge = 0;
    seL4_MessageInfo_t tag = seL4_Recv(command_ep, &badge);
    if (badge || seL4_MessageInfo_get_label(tag) != 0 ||
        seL4_MessageInfo_get_length(tag) < 1 || seL4_GetMR(0) != expected) {
        for (;;) seL4_Yield();
    }
    return tag;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        for (;;) seL4_Yield();
    }

    seL4_CPtr control_ep = (seL4_CPtr)parse_word(argv[0]);
    seL4_CPtr command_ep = (seL4_CPtr)parse_word(argv[1]);
    seL4_Word mode = parse_word(argv[2]);
    seL4_Word private_addr = parse_word(argv[3]);

    send_event(control_ep, LUNA_ISOLATION_EVENT_READY, mode);

    /* Volatile keeps a real relocation to lkl_init in this ELF and proves that
     * the LKL kernel object has moved into the isolated task image. */
    volatile uintptr_t lkl_link_anchor = (uintptr_t)&lkl_init;
    if (!lkl_link_anchor) for (;;) seL4_Yield();
    send_event(control_ep, LUNA_ISOLATION_EVENT_LKL_LINKED, mode);

    struct luna_task_thread_resource resources[LUNA_RESOURCE_SLOTS];
    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        seL4_MessageInfo_t config_tag =
            receive_command(command_ep, LUNA_COMMAND_CONFIGURE_SLOT);
        if (seL4_MessageInfo_get_length(config_tag) != 8 ||
            seL4_GetMR(1) != i || seL4_GetMR(2) != LUNA_RESOURCE_SLOTS) {
            for (;;) seL4_Yield();
        }
        resources[i].tcb = (seL4_CPtr)seL4_GetMR(3);
        resources[i].stack_top = seL4_GetMR(4);
        resources[i].stack_pages = seL4_GetMR(5);
        resources[i].ipc_buffer_addr = seL4_GetMR(6);
        resources[i].join_ntfn = (seL4_CPtr)seL4_GetMR(7);
    }
    if (luna_lkl_task_configure_resources(resources))
        for (;;) seL4_Yield();
    send_event(control_ep, LUNA_ISOLATION_EVENT_RESOURCE_CONFIGURED, mode);

    receive_command(command_ep, LUNA_COMMAND_START);

    if (luna_lkl_task_thread_test())
        for (;;) seL4_Yield();
    send_event(control_ep, LUNA_ISOLATION_EVENT_RESOURCE_OK, mode);

    if (luna_lkl_task_init()) {
        for (;;) seL4_Yield();
    }
    send_event(control_ep, LUNA_ISOLATION_EVENT_LKL_INIT_OK, mode);
    luna_lkl_task_cleanup();

    if (mode == LUNA_ISOLATION_MODE_FAULT) {
        /* The manager maps this address only in its own VSpace.  Reaching the
         * next send would prove that the child can see manager-private data. */
        volatile seL4_Word secret = *(volatile seL4_Word *)(uintptr_t)private_addr;
        send_event(control_ep, LUNA_ISOLATION_EVENT_PRIVATE_PAGE_VISIBLE, secret);
    } else {
        send_event(control_ep, LUNA_ISOLATION_EVENT_DONE, mode);
    }

    /* A sel4utils process receives a cap to its initial TCB in this slot. */
    seL4_TCB_Suspend(SEL4UTILS_TCB_SLOT);
    for (;;) seL4_Yield();
}
