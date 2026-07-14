/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal payload used to establish the process boundary that LKL will move
 * into. It receives a send-only event endpoint, a wait-only command
 * Notification, plus the capabilities installed by sel4utils for its own
 * CSpace, VSpace and TCB.
 */
#include "luna_isolation_protocol.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <lkl_host.h>

/* lkl.o needs these two host-provided symbols.  Full print/panic routing moves
 * with the host operations; for this link-only migration slice they remain
 * deliberately minimal. */
int lkl_printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

void lkl_bug(const char *fmt, ...)
{
    (void)fmt;
    for (;;) seL4_Yield();
}

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

int main(int argc, char **argv)
{
    if (argc != 4) {
        for (;;) seL4_Yield();
    }

    seL4_CPtr control_ep = (seL4_CPtr)parse_word(argv[0]);
    seL4_CPtr command_ntfn = (seL4_CPtr)parse_word(argv[1]);
    seL4_Word mode = parse_word(argv[2]);
    seL4_Word private_addr = parse_word(argv[3]);

    send_event(control_ep, LUNA_ISOLATION_EVENT_READY, mode);

    /* Volatile keeps a real relocation to lkl_init in this ELF and proves that
     * the LKL kernel object has moved into the isolated task image. */
    volatile uintptr_t lkl_link_anchor = (uintptr_t)&lkl_init;
    if (!lkl_link_anchor) for (;;) seL4_Yield();
    send_event(control_ep, LUNA_ISOLATION_EVENT_LKL_LINKED, mode);

    seL4_Word command_badge = 0;
    seL4_Wait(command_ntfn, &command_badge);
    (void)command_badge;

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
