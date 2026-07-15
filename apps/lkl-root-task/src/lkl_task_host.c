/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host operations migrated into luna-lkl-task. Kernel objects are provisioned
 * by the manager; the child owns only bounded TCB/Notification capabilities.
 */
#include "luna_lkl_task_host.h"

#include <sel4/sel4.h>
#include <sel4utils/thread.h>
#include <sys/types.h>
#include <lkl_host.h>
#include <lkl/asm/irq.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>
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
static void task_tls_cleanup(lkl_thread_t tid);

static void task_debug(const char *str)
{
    while (*str) seL4_DebugPutChar(*str++);
}

static struct task_thread_slot *thread_by_tid(lkl_thread_t tid)
{
    if (tid < 2 || tid >= 2 + LUNA_LKL_THREAD_SLOTS) return NULL;
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
    task_tls_cleanup(current_tid);
    seL4_Signal(slot->join_ntfn);
    seL4_TCB_Suspend(slot->self_tcb);
    for (;;) seL4_Yield();
}

static lkl_thread_t task_thread_create(void (*fn)(void *), void *arg)
{
    for (int i = 0; i < LUNA_LKL_THREAD_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        if (slot->used) continue;
        slot->used = 1;
        slot->tid = (lkl_thread_t)(i + 2);
        slot->fn = fn;
        slot->arg = arg;
        if (sel4utils_start_thread(&slot->thread, task_thread_trampoline,
                                   slot, NULL, 1)) {
            slot->used = 0;
            task_debug("luna-lkl-task: host thread start failed\n");
            return 0;
        }
        return slot->tid;
    }
    task_debug("luna-lkl-task: host thread pool exhausted\n");
    return 0;
}

static void task_thread_detach(void) { }

static void task_thread_exit(void)
{
    struct task_thread_slot *slot = thread_by_tid(current_tid);
    if (slot) {
        task_tls_cleanup(current_tid);
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

enum task_sync_kind {
    TASK_SYNC_FREE = 0,
    TASK_SYNC_SEM,
    TASK_SYNC_MUTEX,
};

struct task_sync_slot;
struct lkl_sem { struct task_sync_slot *slot; };
struct lkl_mutex {
    struct task_sync_slot *slot;
    lkl_thread_t owner;
    int count;
    int recursive;
};

struct task_sync_slot {
    seL4_CPtr ntfn;
    volatile int used;
    enum task_sync_kind kind;
    struct lkl_sem sem;
    struct lkl_mutex mutex;
};

static struct task_sync_slot task_sync[LUNA_SYNC_SLOTS];

static void task_sync_drain(seL4_CPtr ntfn)
{
    seL4_Word badge = 0;
    seL4_Poll(ntfn, &badge);
}

static struct task_sync_slot *task_sync_claim(enum task_sync_kind kind)
{
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&task_sync[i].used, &expected, 1,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED))
            continue;
        task_sync[i].kind = kind;
        task_sync_drain(task_sync[i].ntfn);
        return &task_sync[i];
    }
    task_debug("luna-lkl-task: sync pool exhausted\n");
    return NULL;
}

static void task_sync_release(struct task_sync_slot *slot)
{
    if (!slot) return;
    task_sync_drain(slot->ntfn);
    slot->kind = TASK_SYNC_FREE;
    __atomic_store_n(&slot->used, 0, __ATOMIC_RELEASE);
}

static struct lkl_sem *task_sem_alloc(int count)
{
    struct task_sync_slot *slot = task_sync_claim(TASK_SYNC_SEM);
    if (!slot) return NULL;
    slot->sem.slot = slot;
    if (count > 0) seL4_Signal(slot->ntfn);
    return &slot->sem;
}

static void task_sem_free(struct lkl_sem *sem)
{
    if (sem) task_sync_release(sem->slot);
}

static void task_sem_up(struct lkl_sem *sem) { seL4_Signal(sem->slot->ntfn); }
static void task_sem_down(struct lkl_sem *sem)
{
    seL4_Word badge = 0;
    seL4_Wait(sem->slot->ntfn, &badge);
}

static struct lkl_mutex *task_mutex_alloc(int recursive)
{
    struct task_sync_slot *slot = task_sync_claim(TASK_SYNC_MUTEX);
    if (!slot) return NULL;
    slot->mutex.slot = slot;
    slot->mutex.owner = 0;
    slot->mutex.count = 0;
    slot->mutex.recursive = recursive;
    seL4_Signal(slot->ntfn);
    return &slot->mutex;
}

static void task_mutex_free(struct lkl_mutex *mutex)
{
    if (mutex) task_sync_release(mutex->slot);
}

static void task_mutex_lock(struct lkl_mutex *mutex)
{
    if (mutex->recursive && mutex->owner == current_tid) {
        mutex->count++;
        return;
    }
    seL4_Word badge = 0;
    seL4_Wait(mutex->slot->ntfn, &badge);
    mutex->owner = current_tid;
    mutex->count = 1;
}

static void task_mutex_unlock(struct lkl_mutex *mutex)
{
    if (mutex->owner != current_tid) return;
    if (mutex->recursive && --mutex->count > 0) return;
    mutex->owner = 0;
    mutex->count = 0;
    seL4_Signal(mutex->slot->ntfn);
}

#define TASK_TLS_KEYS 16
struct lkl_tls_key {
    volatile int used;
    void *data[LUNA_RESOURCE_SLOTS + 2];
    void (*destructor)(void *);
};
static struct lkl_tls_key task_tls[TASK_TLS_KEYS];

static void task_tls_cleanup(lkl_thread_t tid)
{
    if (tid >= LUNA_RESOURCE_SLOTS + 2) return;
    for (int i = 0; i < TASK_TLS_KEYS; i++) {
        void *data = task_tls[i].data[tid];
        task_tls[i].data[tid] = NULL;
        if (data && task_tls[i].used && task_tls[i].destructor)
            task_tls[i].destructor(data);
    }
}

static struct lkl_tls_key *task_tls_alloc(void (*destructor)(void *))
{
    for (int i = 0; i < TASK_TLS_KEYS; i++) {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&task_tls[i].used, &expected, 1,
                                         false, __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED))
            continue;
        memset(task_tls[i].data, 0, sizeof(task_tls[i].data));
        task_tls[i].destructor = destructor;
        return &task_tls[i];
    }
    return NULL;
}

static void task_tls_free(struct lkl_tls_key *key)
{
    if (!key) return;
    for (int i = 0; i < LUNA_RESOURCE_SLOTS + 2; i++) {
        void *data = key->data[i];
        key->data[i] = NULL;
        if (data && key->destructor) key->destructor(data);
    }
    key->destructor = NULL;
    __atomic_store_n(&key->used, 0, __ATOMIC_RELEASE);
}

static int task_tls_set(struct lkl_tls_key *key, void *data)
{
    if (!key || current_tid >= LUNA_RESOURCE_SLOTS + 2) return -1;
    key->data[current_tid] = data;
    return 0;
}

static void *task_tls_get(struct lkl_tls_key *key)
{
    if (!key || current_tid >= LUNA_RESOURCE_SLOTS + 2) return NULL;
    return key->data[current_tid];
}

static unsigned char task_heap[32 * 1024 * 1024]
    __attribute__((aligned(4096)));
static size_t task_heap_off;

static void *task_bump_alloc(unsigned long size, unsigned long align)
{
    uintptr_t base = (uintptr_t)task_heap + task_heap_off;
    base = (base + align - 1) & ~(uintptr_t)(align - 1);
    size_t needed = size ? size : 1;
    if (base + needed > (uintptr_t)task_heap + sizeof(task_heap)) {
        task_debug("luna-lkl-task: host heap exhausted\n");
        return NULL;
    }
    task_heap_off = (size_t)(base + needed - (uintptr_t)task_heap);
    return (void *)base;
}

static void *task_mem_alloc(unsigned long size) { return task_bump_alloc(size, 16); }
static void task_mem_free(void *ptr) { (void)ptr; }
static void *task_page_alloc(unsigned long size)
{
    return task_bump_alloc(size ? size : 4096, 4096);
}
static void task_page_free(void *ptr, unsigned long size) { (void)ptr; (void)size; }
static void *task_mmap(void *addr, unsigned long size, enum lkl_prot prot)
{
    (void)addr;
    (void)prot;
    return task_bump_alloc(size, 4096);
}
static int task_munmap(void *addr, unsigned long size) { (void)addr; (void)size; return 0; }
static void task_shmem_init(unsigned long size) { (void)size; }
static void *task_shmem_mmap(void *addr, unsigned long page_offset,
                            unsigned long size, enum lkl_prot prot)
{
    (void)addr;
    (void)page_offset;
    (void)prot;
    return task_bump_alloc(size, 4096);
}

static void task_jmp_buf_set(struct lkl_jmp_buf *buffer, void (*fn)(void))
{
    if (!setjmp(*(jmp_buf *)buffer->buf)) fn();
}

static void task_jmp_buf_longjmp(struct lkl_jmp_buf *buffer, int value)
{
    longjmp(*(jmp_buf *)buffer->buf, value);
}

static unsigned long long task_time_epoch;
static unsigned long long task_tsc_frequency;
static unsigned long long task_time(void)
{
    unsigned long long cycles = __builtin_ia32_rdtsc() - task_time_epoch;
    if (!task_tsc_frequency) return 0;
    return (cycles / task_tsc_frequency) * 1000000000ULL +
           ((cycles % task_tsc_frequency) * 1000000000ULL) /
               task_tsc_frequency;
}

#define TASK_TIMER_TID 0x7ffffffdUL
static void (*task_timer_fn)(void);
static volatile int task_timer_started;
static char task_timer_handle;
static volatile int task_timer_armed;
static unsigned long long task_timer_deadline_ns;

static void task_timer_service(void *arg)
{
    (void)arg;
    for (;;) {
        if (__atomic_load_n(&task_timer_armed, __ATOMIC_ACQUIRE)) {
            unsigned long long deadline =
                __atomic_load_n(&task_timer_deadline_ns, __ATOMIC_RELAXED);
            if (task_time() >= deadline &&
                __atomic_exchange_n(&task_timer_armed, 0,
                                    __ATOMIC_ACQ_REL)) {
                void (*fn)(void) = task_timer_fn;
                if (fn) fn();
            }
        }
        seL4_Yield();
    }
}

static void *task_timer_alloc(void (*fn)(void))
{
    if (!fn || !task_tsc_frequency) return NULL;
    task_timer_fn = fn;
    __atomic_store_n(&task_timer_armed, 0, __ATOMIC_RELEASE);
    if (!task_timer_started) {
        struct task_thread_slot *slot = &task_threads[LUNA_TIMER_SLOT];
        if (slot->used) return NULL;
        slot->used = 1;
        slot->tid = TASK_TIMER_TID;
        slot->fn = task_timer_service;
        slot->arg = NULL;
        if (sel4utils_start_thread(&slot->thread, task_thread_trampoline,
                                   slot, NULL, 1)) {
            slot->used = 0;
            slot->tid = 0;
            slot->fn = NULL;
            slot->arg = NULL;
            return NULL;
        }
        task_timer_started = 1;
    }
    return &task_timer_handle;
}

static int task_timer_set_oneshot(void *timer, unsigned long ns)
{
    if (timer != &task_timer_handle || !task_timer_started) return -1;
    __atomic_store_n(&task_timer_deadline_ns,
                     task_time() + (ns ? ns : 1), __ATOMIC_RELAXED);
    __atomic_store_n(&task_timer_armed, 1, __ATOMIC_RELEASE);
    return 0;
}

static void task_timer_free(void *timer)
{
    if (timer == &task_timer_handle)
        __atomic_store_n(&task_timer_armed, 0, __ATOMIC_RELEASE);
}

static void task_print(const char *str, int length)
{
    for (int i = 0; i < length; i++) seL4_DebugPutChar(str[i]);
}

static void task_panic(void)
{
    for (;;) seL4_Yield();
}

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
    .print = task_print,
    .panic = task_panic,
    .sem_alloc = task_sem_alloc,
    .sem_free = task_sem_free,
    .sem_up = task_sem_up,
    .sem_down = task_sem_down,
    .mutex_alloc = task_mutex_alloc,
    .mutex_free = task_mutex_free,
    .mutex_lock = task_mutex_lock,
    .mutex_unlock = task_mutex_unlock,
    .thread_create = task_thread_create,
    .thread_detach = task_thread_detach,
    .thread_exit = task_thread_exit,
    .thread_join = task_thread_join,
    .thread_self = task_thread_self,
    .thread_equal = task_thread_equal,
    .thread_stack = task_thread_stack,
    .tls_alloc = task_tls_alloc,
    .tls_free = task_tls_free,
    .tls_set = task_tls_set,
    .tls_get = task_tls_get,
    .mem_alloc = task_mem_alloc,
    .mem_free = task_mem_free,
    .page_alloc = task_page_alloc,
    .page_free = task_page_free,
    .time = task_time,
    .timer_alloc = task_timer_alloc,
    .timer_set_oneshot = task_timer_set_oneshot,
    .timer_free = task_timer_free,
    .jmp_buf_set = task_jmp_buf_set,
    .jmp_buf_longjmp = task_jmp_buf_longjmp,
    .memcpy = task_memcpy,
    .memset = task_memset,
    .memmove = task_memmove,
    .mmap = task_mmap,
    .munmap = task_munmap,
    .shmem_init = task_shmem_init,
    .shmem_mmap = task_shmem_mmap,
};

int luna_lkl_task_configure_resources(
    const struct luna_task_thread_resource resources[LUNA_RESOURCE_SLOTS],
    const struct luna_task_sync_resource sync[LUNA_SYNC_SLOTS])
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
    memset(task_sync, 0, sizeof(task_sync));
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        task_sync[i].ntfn = sync[i].ntfn;
        if (!task_sync[i].ntfn) return -1;
        task_sync_drain(task_sync[i].ntfn);
    }
    return 0;
}

static __thread seL4_Word thread_test_tls;
static volatile seL4_Word thread_test_results[LUNA_LKL_THREAD_SLOTS];

static void thread_test_worker(void *arg)
{
    seL4_Word index = (seL4_Word)(uintptr_t)arg;
    thread_test_tls = LUNA_RESOURCE_TLS_VALUE + index;
    thread_test_results[index] = thread_test_tls;
}

int luna_lkl_task_thread_test(void)
{
    lkl_thread_t tids[LUNA_LKL_THREAD_SLOTS];
    memset((void *)thread_test_results, 0, sizeof(thread_test_results));
    for (seL4_Word i = 0; i < LUNA_LKL_THREAD_SLOTS; i++) {
        tids[i] = task_thread_create(thread_test_worker, (void *)(uintptr_t)i);
        if (!tids[i]) return -1;
    }
    for (seL4_Word i = 0; i < LUNA_LKL_THREAD_SLOTS; i++) {
        if (task_thread_join(tids[i])) return -1;
        if (thread_test_results[i] != LUNA_RESOURCE_TLS_VALUE + i) return -1;
    }
    return 0;
}

int lkl_printf(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    int length = result < 0 ? 0 :
                 (result < (int)sizeof(buffer) ? result : (int)sizeof(buffer) - 1);
    if (length) task_print(buffer, length);
    return result;
}

void lkl_bug(const char *fmt, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    int length = result < 0 ? 0 :
                 (result < (int)sizeof(buffer) ? result : (int)sizeof(buffer) - 1);
    if (length) task_print(buffer, length);
    task_panic();
}

int luna_lkl_task_init(void)
{
    task_heap_off = 0;
    memset(task_tls, 0, sizeof(task_tls));
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        task_sync_drain(task_sync[i].ntfn);
        task_sync[i].used = 0;
        task_sync[i].kind = TASK_SYNC_FREE;
    }
    task_time_epoch = __builtin_ia32_rdtsc();
    return lkl_init(&task_host_ops);
}

int luna_lkl_task_start_kernel(unsigned long long tsc_frequency)
{
    if (!tsc_frequency) return -1;
    task_tsc_frequency = tsc_frequency;
    task_time_epoch = __builtin_ia32_rdtsc();
    return lkl_start_kernel("mem=16M loglevel=4");
}

long luna_lkl_task_halt(void)
{
    return lkl_sys_halt();
}
