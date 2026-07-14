/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host operations migrated into luna-lkl-task.  This first slice is sufficient
 * for lkl_init()/lkl_cleanup(); thread, synchronization, time and device ops
 * are added before moving lkl_start_kernel().
 */
#include "luna_lkl_task_host.h"

#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <sys/types.h>
#include <lkl_host.h>
#include <stdint.h>
#include <string.h>

struct task_thread_slot {
    sel4utils_thread_t thread;
    seL4_CPtr self_tcb;
    seL4_CPtr join_ntfn;
    void (*fn)(void *);
    void *arg;
    lkl_thread_t tid;
    int used;
};

static struct task_thread_slot task_threads[LUNA_RESOURCE_SLOTS];
static __thread lkl_thread_t current_tid = 1;

static struct task_thread_slot *thread_by_tid(lkl_thread_t tid)
{
    if (tid < 2 || tid >= 2 + LUNA_RESOURCE_SLOTS) return NULL;
    struct task_thread_slot *slot = &task_threads[tid - 2];
    return slot->used && slot->tid == tid ? slot : NULL;
}

static void task_thread_trampoline(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct task_thread_slot *slot = arg0;
    current_tid = slot->tid;
    slot->fn(slot->arg);
    seL4_Signal(slot->join_ntfn);
    seL4_TCB_Suspend(slot->self_tcb);
    for (;;) seL4_Yield();
}

static lkl_thread_t task_thread_create(void (*fn)(void *), void *arg)
{
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        if (slot->used) continue;
        slot->used = 1;
        slot->tid = (lkl_thread_t)(i + 2);
        slot->fn = fn;
        slot->arg = arg;
        if (sel4utils_start_thread(&slot->thread, task_thread_trampoline,
                                   slot, NULL, 1)) {
            slot->used = 0;
            return 0;
        }
        return slot->tid;
    }
    return 0;
}

static void task_thread_detach(void) { }

static void task_thread_exit(void)
{
    struct task_thread_slot *slot = thread_by_tid(current_tid);
    if (slot) {
        seL4_Signal(slot->join_ntfn);
        seL4_TCB_Suspend(slot->self_tcb);
    }
    for (;;) seL4_Yield();
}

static int task_thread_join(lkl_thread_t tid)
{
    if (tid == 1) return 0;
    struct task_thread_slot *slot = thread_by_tid(tid);
    if (!slot) return -1;
    seL4_Word badge = 0;
    seL4_Wait(slot->join_ntfn, &badge);
    slot->fn = NULL;
    slot->arg = NULL;
    slot->used = 0;
    return 0;
}

static lkl_thread_t task_thread_self(void) { return current_tid; }
static int task_thread_equal(lkl_thread_t a, lkl_thread_t b) { return a == b; }
static void *task_thread_stack(unsigned long *size) { (void)size; return NULL; }

static void *task_memcpy(void *dest, const void *src, unsigned long count)
{
    return memcpy(dest, src, count);
}

static void *task_memset(void *dest, int value, unsigned long count)
{
    return memset(dest, value, count);
}

static void *task_memmove(void *dest, const void *src, unsigned long count)
{
    return memmove(dest, src, count);
}

static struct lkl_host_operations task_host_ops = {
    .thread_create = task_thread_create,
    .thread_detach = task_thread_detach,
    .thread_exit = task_thread_exit,
    .thread_join = task_thread_join,
    .thread_self = task_thread_self,
    .thread_equal = task_thread_equal,
    .thread_stack = task_thread_stack,
    .memcpy = task_memcpy,
    .memset = task_memset,
    .memmove = task_memmove,
};

int luna_lkl_task_configure_resources(
    const struct luna_task_thread_resource resources[LUNA_RESOURCE_SLOTS])
{
    memset(task_threads, 0, sizeof(task_threads));
    current_tid = 1;
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        slot->self_tcb = resources[i].tcb;
        slot->join_ntfn = resources[i].join_ntfn;
        slot->thread.tcb.cptr = resources[i].tcb;
        slot->thread.stack_top = (void *)(uintptr_t)resources[i].stack_top;
        slot->thread.initial_stack_pointer = slot->thread.stack_top;
        slot->thread.stack_size = resources[i].stack_pages;
        slot->thread.ipc_buffer_addr = resources[i].ipc_buffer_addr;
        if (!slot->self_tcb || !slot->join_ntfn || !slot->thread.stack_top ||
            !slot->thread.stack_size || !slot->thread.ipc_buffer_addr)
            return -1;
    }
    return 0;
}

static __thread seL4_Word thread_test_tls;
static volatile seL4_Word thread_test_results[LUNA_RESOURCE_SLOTS];

static void thread_test_worker(void *arg)
{
    seL4_Word index = (seL4_Word)(uintptr_t)arg;
    thread_test_tls = LUNA_RESOURCE_TLS_VALUE + index;
    thread_test_results[index] = thread_test_tls;
}

int luna_lkl_task_thread_test(void)
{
    lkl_thread_t tids[LUNA_RESOURCE_SLOTS];
    memset((void *)thread_test_results, 0, sizeof(thread_test_results));
    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        tids[i] = task_thread_create(thread_test_worker, (void *)(uintptr_t)i);
        if (!tids[i]) return -1;
    }
    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        if (task_thread_join(tids[i])) return -1;
        if (thread_test_results[i] != LUNA_RESOURCE_TLS_VALUE + i) return -1;
    }
    return 0;
}

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

int luna_lkl_task_init(void)
{
    return lkl_init(&task_host_ops);
}

void luna_lkl_task_cleanup(void)
{
    lkl_cleanup();
}
