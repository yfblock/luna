/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal payload used to establish the process boundary that LKL will move
 * into. It receives a send-only event endpoint, a wait-only command
 * Notification, plus the capabilities installed by sel4utils for its own
 * CSpace, VSpace and TCB.
 */
#include "luna_isolation_protocol.h"
#include "luna_lkl_task_host.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/thread.h>
#include <lkl_host.h>

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

struct child_worker_descriptor {
    sel4utils_thread_t thread;
    seL4_CPtr self_tcb;
    seL4_CPtr join_ntfn;
};

static __thread seL4_Word child_worker_tls;
static volatile seL4_Word child_worker_result;

static void child_worker(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct child_worker_descriptor *worker = arg0;

    child_worker_tls = LUNA_RESOURCE_TLS_VALUE;
    child_worker_result = child_worker_tls;
    seL4_Signal(worker->join_ntfn);
    seL4_TCB_Suspend(worker->self_tcb);
    for (;;) seL4_Yield();
}

int main(int argc, char **argv)
{
    if (argc != 10) {
        for (;;) seL4_Yield();
    }

    seL4_CPtr control_ep = (seL4_CPtr)parse_word(argv[0]);
    seL4_CPtr command_ntfn = (seL4_CPtr)parse_word(argv[1]);
    seL4_Word mode = parse_word(argv[2]);
    seL4_Word private_addr = parse_word(argv[3]);
    seL4_Word resource_slots = parse_word(argv[4]);
    if (resource_slots != LUNA_RESOURCE_SLOTS) {
        for (;;) seL4_Yield();
    }

    struct child_worker_descriptor worker = {0};
    worker.self_tcb = (seL4_CPtr)parse_word(argv[5]);
    worker.thread.tcb.cptr = worker.self_tcb;
    worker.thread.stack_top = (void *)(uintptr_t)parse_word(argv[6]);
    worker.thread.initial_stack_pointer = worker.thread.stack_top;
    worker.thread.stack_size = (size_t)parse_word(argv[7]);
    worker.thread.ipc_buffer_addr = parse_word(argv[8]);
    worker.join_ntfn = (seL4_CPtr)parse_word(argv[9]);

    send_event(control_ep, LUNA_ISOLATION_EVENT_READY, mode);

    /* Volatile keeps a real relocation to lkl_init in this ELF and proves that
     * the LKL kernel object has moved into the isolated task image. */
    volatile uintptr_t lkl_link_anchor = (uintptr_t)&lkl_init;
    if (!lkl_link_anchor) for (;;) seL4_Yield();
    send_event(control_ep, LUNA_ISOLATION_EVENT_LKL_LINKED, mode);

    seL4_Word command_badge = 0;
    seL4_Wait(command_ntfn, &command_badge);
    (void)command_badge;

    child_worker_result = 0;
    if (sel4utils_start_thread(&worker.thread, child_worker, &worker, NULL, 1)) {
        for (;;) seL4_Yield();
    }
    seL4_Word join_badge = 0;
    seL4_Wait(worker.join_ntfn, &join_badge);
    (void)join_badge;
    if (child_worker_result != LUNA_RESOURCE_TLS_VALUE) {
        for (;;) seL4_Yield();
    }
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
