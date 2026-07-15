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
#include <limits.h>

void *lkl_ioremap(long addr, int size);
int lkl_iomem_access(const volatile void *addr, void *res, int size,
                     int write);

struct task_thread_slot {
    sel4utils_thread_t thread;
    seL4_CPtr self_tcb;
    seL4_CPtr join_ntfn;
    void (*fn)(void *);
    void *arg;
    lkl_thread_t tid;
    unsigned tls_index;
    volatile int used;
    volatile int exited;
    volatile int join_claimed;
};

static struct task_thread_slot task_threads[LUNA_RESOURCE_SLOTS];
static __thread lkl_thread_t current_tid = 1;
static __thread unsigned current_tls_index = 1;
static volatile lkl_thread_t task_next_tid = 2;
static volatile int task_thread_table_lock_word;
static void task_tls_cleanup(unsigned tls_index);

static void task_debug(const char *str)
{
    while (*str) seL4_DebugPutChar(*str++);
}

static void task_thread_table_lock(void)
{
    while (__atomic_test_and_set(&task_thread_table_lock_word,
                                 __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void task_thread_table_unlock(void)
{
    __atomic_clear(&task_thread_table_lock_word, __ATOMIC_RELEASE);
}

static struct task_thread_slot *thread_by_tid_unlocked(lkl_thread_t tid)
{
    if (tid < 2) return NULL;
    for (int i = 0; i < LUNA_LKL_THREAD_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        if (__atomic_load_n(&slot->used, __ATOMIC_ACQUIRE) &&
            slot->tid == tid)
            return slot;
    }
    return NULL;
}

static struct task_thread_slot *thread_by_tid(lkl_thread_t tid)
{
    struct task_thread_slot *slot;
    task_thread_table_lock();
    slot = thread_by_tid_unlocked(tid);
    task_thread_table_unlock();
    return slot;
}

static void task_thread_drain_join(struct task_thread_slot *slot)
{
    seL4_Word badge = 0;
    seL4_Poll(slot->join_ntfn, &badge);
}

static __attribute__((noreturn)) void task_thread_complete(
    struct task_thread_slot *slot)
{
    task_tls_cleanup(slot->tls_index);
    __atomic_store_n(&slot->exited, 1, __ATOMIC_RELEASE);
    seL4_Signal(slot->join_ntfn);
    seL4_TCB_Suspend(slot->self_tcb);
    for (;;) seL4_Yield();
}

static void task_thread_trampoline(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct task_thread_slot *slot = arg0;
    current_tid = slot->tid;
    current_tls_index = slot->tls_index;
    slot->fn(slot->arg);
    task_thread_complete(slot);
}

static lkl_thread_t task_thread_create(void (*fn)(void *), void *arg)
{
    struct task_thread_slot *selected = NULL;
    task_thread_table_lock();
    for (int i = 0; i < LUNA_LKL_THREAD_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        if (slot->used) continue;
        slot->used = 1;
        task_thread_drain_join(slot);
        slot->tid = __atomic_fetch_add(&task_next_tid, 1,
                                       __ATOMIC_RELAXED);
        if (slot->tid < 2) slot->tid = 2;
        slot->tls_index = (unsigned)i + 2;
        slot->fn = fn;
        slot->arg = arg;
        slot->exited = 0;
        slot->join_claimed = 0;
        selected = slot;
        break;
    }
    task_thread_table_unlock();
    if (!selected) {
        task_debug("luna-lkl-task: host thread pool exhausted\n");
        return 0;
    }
    if (sel4utils_start_thread(&selected->thread, task_thread_trampoline,
                               selected, NULL, 1)) {
        task_thread_table_lock();
        selected->tid = 0;
        selected->fn = NULL;
        selected->arg = NULL;
        selected->used = 0;
        task_thread_table_unlock();
        task_debug("luna-lkl-task: host thread start failed\n");
        return 0;
    }
    return selected->tid;
}

static void task_thread_detach(void) { }

static void task_thread_exit(void)
{
    struct task_thread_slot *slot = thread_by_tid(current_tid);
    if (slot) task_thread_complete(slot);
    for (;;) seL4_Yield();
}

static int task_thread_join(lkl_thread_t tid)
{
    if (tid == 1) return 0;
    task_thread_table_lock();
    struct task_thread_slot *slot = thread_by_tid_unlocked(tid);
    if (!slot || slot->join_claimed) {
        task_thread_table_unlock();
        return -1;
    }
    slot->join_claimed = 1;
    task_thread_table_unlock();
    seL4_Word badge = 0;
    if (!__atomic_load_n(&slot->exited, __ATOMIC_ACQUIRE))
        seL4_Wait(slot->join_ntfn, &badge);
    else
        seL4_Poll(slot->join_ntfn, &badge);
    task_thread_table_lock();
    if (!slot->used || slot->tid != tid) {
        task_thread_table_unlock();
        return -1;
    }
    slot->fn = NULL;
    slot->arg = NULL;
    slot->tid = 0;
    slot->exited = 0;
    slot->used = 0;
    task_thread_table_unlock();
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
struct lkl_sem {
    struct task_sync_slot *slot;
    volatile int count;
    volatile int waiters;
};
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
    if (count < 0) return NULL;
    struct task_sync_slot *slot = task_sync_claim(TASK_SYNC_SEM);
    if (!slot) return NULL;
    slot->sem.slot = slot;
    slot->sem.count = count;
    slot->sem.waiters = 0;
    return &slot->sem;
}

static void task_sem_free(struct lkl_sem *sem)
{
    if (!sem) return;
    if (__atomic_load_n(&sem->waiters, __ATOMIC_ACQUIRE) != 0) {
        task_debug("luna-lkl-task: freeing semaphore with waiters\n");
        return;
    }
    task_sync_release(sem->slot);
}

static int task_sem_try_down(struct lkl_sem *sem)
{
    int count = __atomic_load_n(&sem->count, __ATOMIC_ACQUIRE);
    while (count > 0) {
        if (__atomic_compare_exchange_n(&sem->count, &count, count - 1,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}

static void task_sem_up(struct lkl_sem *sem)
{
    int count = __atomic_load_n(&sem->count, __ATOMIC_ACQUIRE);
    for (;;) {
        if (count == INT_MAX) {
            task_debug("luna-lkl-task: semaphore count overflow\n");
            return;
        }
        if (__atomic_compare_exchange_n(&sem->count, &count, count + 1,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            break;
    }
    if (__atomic_load_n(&sem->waiters, __ATOMIC_ACQUIRE) > 0)
        seL4_Signal(sem->slot->ntfn);
}

static void task_sem_down(struct lkl_sem *sem)
{
    if (task_sem_try_down(sem)) return;
    __atomic_fetch_add(&sem->waiters, 1, __ATOMIC_ACQ_REL);
    for (;;) {
        if (task_sem_try_down(sem)) {
            int remaining = __atomic_sub_fetch(&sem->waiters, 1,
                                               __ATOMIC_ACQ_REL);
            if (remaining > 0 &&
                __atomic_load_n(&sem->count, __ATOMIC_ACQUIRE) > 0)
                seL4_Signal(sem->slot->ntfn);
            return;
        }
        seL4_Word badge = 0;
        seL4_Wait(sem->slot->ntfn, &badge);
    }
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
    if (!mutex) return;
    if (mutex->owner || mutex->count) {
        task_debug("luna-lkl-task: freeing owned mutex\n");
        return;
    }
    task_sync_release(mutex->slot);
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

static volatile unsigned task_mutex_owner_errors;

static int task_mutex_unlock_checked(struct lkl_mutex *mutex)
{
    if (mutex->owner != current_tid || mutex->count <= 0) {
        __atomic_fetch_add(&task_mutex_owner_errors, 1, __ATOMIC_RELAXED);
        return -1;
    }
    if (mutex->recursive && --mutex->count > 0) return 0;
    mutex->owner = 0;
    mutex->count = 0;
    seL4_Signal(mutex->slot->ntfn);
    return 0;
}

static void task_mutex_unlock(struct lkl_mutex *mutex)
{
    if (task_mutex_unlock_checked(mutex))
        task_debug("luna-lkl-task: mutex unlock by non-owner\n");
}

#define TASK_TLS_KEYS 16
#define TASK_TLS_DESTRUCTOR_ITERATIONS 4
struct lkl_tls_key {
    volatile int used;
    void *data[LUNA_RESOURCE_SLOTS + 2];
    void (*destructor)(void *);
};
static struct lkl_tls_key task_tls[TASK_TLS_KEYS];

static void task_tls_cleanup(unsigned tls_index)
{
    if (tls_index >= LUNA_RESOURCE_SLOTS + 2) return;
    for (int pass = 0; pass < TASK_TLS_DESTRUCTOR_ITERATIONS; pass++) {
        int called = 0;
        for (int i = 0; i < TASK_TLS_KEYS; i++) {
            struct lkl_tls_key *key = &task_tls[i];
            void *data = key->data[tls_index];
            key->data[tls_index] = NULL;
            void (*destructor)(void *) = key->destructor;
            if (data && __atomic_load_n(&key->used, __ATOMIC_ACQUIRE) &&
                destructor) {
                called = 1;
                destructor(data);
            }
        }
        if (!called) break;
    }
    for (int i = 0; i < TASK_TLS_KEYS; i++)
        task_tls[i].data[tls_index] = NULL;
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
    __atomic_store_n(&key->used, 0, __ATOMIC_RELEASE);
    memset(key->data, 0, sizeof(key->data));
    key->destructor = NULL;
}

static int task_tls_set(struct lkl_tls_key *key, void *data)
{
    if (!key || !__atomic_load_n(&key->used, __ATOMIC_ACQUIRE) ||
        current_tls_index >= LUNA_RESOURCE_SLOTS + 2)
        return -1;
    key->data[current_tls_index] = data;
    return 0;
}

static void *task_tls_get(struct lkl_tls_key *key)
{
    if (!key || !__atomic_load_n(&key->used, __ATOMIC_ACQUIRE) ||
        current_tls_index >= LUNA_RESOURCE_SLOTS + 2)
        return NULL;
    return key->data[current_tls_index];
}

#define TASK_HEAP_ALIGNMENT       16UL
#define TASK_PAGE_SIZE            4096UL
#define TASK_HEAP_MAX_ALLOCATIONS 1024

struct task_heap_allocation {
    void *address;
    size_t pages;
    size_t requested;
    int used;
};

static unsigned char task_heap_pages[LUNA_CHILD_HEAP_PAGES];
static struct task_heap_allocation
    task_heap_allocations[TASK_HEAP_MAX_ALLOCATIONS];
static seL4_CPtr task_manager_control_ep;
static seL4_CPtr task_manager_command_ep;
static volatile int task_manager_lock_word;
static size_t task_heap_active_allocations;
static size_t task_heap_active_bytes;
static size_t task_heap_peak_bytes;

void luna_lkl_task_manager_lock(void)
{
    while (__atomic_test_and_set(&task_manager_lock_word, __ATOMIC_ACQUIRE))
        seL4_Yield();
}

void luna_lkl_task_manager_unlock(void)
{
    __atomic_clear(&task_manager_lock_word, __ATOMIC_RELEASE);
}

static void task_heap_init(void)
{
    task_manager_lock_word = 0;
    memset(task_heap_pages, 0, sizeof(task_heap_pages));
    memset(task_heap_allocations, 0, sizeof(task_heap_allocations));
    task_heap_active_allocations = 0;
    task_heap_active_bytes = 0;
    task_heap_peak_bytes = 0;
}

int luna_lkl_task_manager_request_value(enum luna_isolation_event event,
                                        seL4_Word value1,
                                        seL4_Word value2,
                                        seL4_Word *response_value)
{
    if (!task_manager_control_ep || !task_manager_command_ep) return -1;
    seL4_SetMR(0, event);
    seL4_SetMR(1, value1);
    seL4_SetMR(2, value2);
    seL4_Send(task_manager_control_ep,
              seL4_MessageInfo_new(0, 0, 0, 3));

    seL4_Word badge = 0;
    seL4_MessageInfo_t tag = seL4_Recv(task_manager_command_ep, &badge);
    seL4_Word expected;
    seL4_Word expected_length = 2;
    if (event == LUNA_ISOLATION_EVENT_MEMORY_MAP ||
        event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP) {
        expected = LUNA_COMMAND_MEMORY_RESULT;
    } else if (event == LUNA_ISOLATION_EVENT_NET_TX ||
               event == LUNA_ISOLATION_EVENT_NET_RX ||
               event == LUNA_ISOLATION_EVENT_NET_WAKE ||
               event == LUNA_ISOLATION_EVENT_NET_CONTROL ||
               event == LUNA_ISOLATION_EVENT_NET_STATS ||
               event == LUNA_ISOLATION_EVENT_NET_TX_STATS ||
               event == LUNA_ISOLATION_EVENT_NET_TX_STRESS) {
        expected = LUNA_COMMAND_NET_RESULT;
        expected_length = 3;
    } else {
        expected = LUNA_COMMAND_DISK_RESULT;
    }
    if (badge || seL4_MessageInfo_get_label(tag) != 0 ||
        seL4_MessageInfo_get_length(tag) != expected_length ||
        seL4_GetMR(0) != expected)
        return -1;
    if (response_value)
        *response_value = expected_length > 2 ? seL4_GetMR(2) : 0;
    return (int)seL4_GetMR(1);
}

int luna_lkl_task_manager_request(enum luna_isolation_event event,
                                  seL4_Word value1, seL4_Word value2)
{
    return luna_lkl_task_manager_request_value(event, value1, value2, NULL);
}

static int task_heap_request(enum luna_isolation_event event, void *address,
                             size_t pages)
{
    if (!pages) return -1;
    return luna_lkl_task_manager_request(
        event, (seL4_Word)(uintptr_t)address, pages);
}

static void *task_heap_alloc(unsigned long size, unsigned long align, int zero)
{
    if (align < TASK_HEAP_ALIGNMENT) align = TASK_HEAP_ALIGNMENT;
    if ((align & (align - 1)) || align > TASK_PAGE_SIZE) return NULL;
    size_t requested = size ? (size_t)size : 1;
    if (requested > SIZE_MAX - (TASK_PAGE_SIZE - 1)) return NULL;
    size_t pages = (requested + TASK_PAGE_SIZE - 1) / TASK_PAGE_SIZE;
    if (!pages || pages > LUNA_CHILD_HEAP_PAGES) return NULL;

    luna_lkl_task_manager_lock();
    int record_index = -1;
    for (int i = 0; i < TASK_HEAP_MAX_ALLOCATIONS; i++) {
        if (!task_heap_allocations[i].used) {
            record_index = i;
            break;
        }
    }
    size_t first_page = LUNA_CHILD_HEAP_PAGES;
    if (record_index >= 0) {
        for (size_t i = 0; i + pages <= LUNA_CHILD_HEAP_PAGES;) {
            size_t j = 0;
            while (j < pages && !task_heap_pages[i + j]) j++;
            if (j == pages) {
                first_page = i;
                break;
            }
            i += j + 1;
        }
    }
    if (record_index < 0 || first_page == LUNA_CHILD_HEAP_PAGES) {
        luna_lkl_task_manager_unlock();
        task_debug("luna-lkl-task: host heap exhausted\n");
        return NULL;
    }

    void *address = (void *)(uintptr_t)(LUNA_CHILD_HEAP_BASE +
                                        first_page * TASK_PAGE_SIZE);
    if (task_heap_request(LUNA_ISOLATION_EVENT_MEMORY_MAP, address, pages)) {
        luna_lkl_task_manager_unlock();
        task_debug("luna-lkl-task: host page map failed\n");
        return NULL;
    }

    memset(&task_heap_pages[first_page], 1, pages);
    struct task_heap_allocation *record =
        &task_heap_allocations[record_index];
    record->address = address;
    record->pages = pages;
    record->requested = requested;
    record->used = 1;
    task_heap_active_allocations++;
    task_heap_active_bytes += requested;
    if (task_heap_active_bytes > task_heap_peak_bytes)
        task_heap_peak_bytes = task_heap_active_bytes;
    luna_lkl_task_manager_unlock();
    if (zero) memset(address, 0, requested);
    return address;
}

static int task_heap_free(void *ptr)
{
    if (!ptr) return 0;
    uintptr_t address = (uintptr_t)ptr;
    uintptr_t heap_start = LUNA_CHILD_HEAP_BASE;
    uintptr_t heap_end = heap_start + LUNA_CHILD_HEAP_SIZE;
    if (address < heap_start || address >= heap_end ||
        (address & (TASK_PAGE_SIZE - 1))) {
        task_debug("luna-lkl-task: invalid host heap free\n");
        return -1;
    }

    luna_lkl_task_manager_lock();
    struct task_heap_allocation *record = NULL;
    for (int i = 0; i < TASK_HEAP_MAX_ALLOCATIONS; i++) {
        if (task_heap_allocations[i].used &&
            task_heap_allocations[i].address == ptr) {
            record = &task_heap_allocations[i];
            break;
        }
    }
    if (!record) {
        luna_lkl_task_manager_unlock();
        task_debug("luna-lkl-task: corrupt host heap free\n");
        return -1;
    }

    if (task_heap_request(LUNA_ISOLATION_EVENT_MEMORY_UNMAP, ptr,
                          record->pages)) {
        luna_lkl_task_manager_unlock();
        task_debug("luna-lkl-task: host page unmap failed\n");
        return -1;
    }
    size_t first_page = (address - heap_start) / TASK_PAGE_SIZE;
    memset(&task_heap_pages[first_page], 0, record->pages);
    task_heap_active_allocations--;
    task_heap_active_bytes -= record->requested;
    memset(record, 0, sizeof(*record));
    luna_lkl_task_manager_unlock();
    return 0;
}

static int task_heap_is_idle(void)
{
    int idle;
    luna_lkl_task_manager_lock();
    idle = task_heap_active_allocations == 0 && task_heap_active_bytes == 0;
    if (idle) {
        for (size_t i = 0; i < LUNA_CHILD_HEAP_PAGES; i++) {
            if (task_heap_pages[i]) {
                idle = 0;
                break;
            }
        }
    }
    luna_lkl_task_manager_unlock();
    return idle;
}

static void *task_mem_alloc(unsigned long size)
{
    return task_heap_alloc(size, TASK_HEAP_ALIGNMENT, 0);
}

static void task_mem_free(void *ptr)
{
    (void)task_heap_free(ptr);
}

static void *task_page_alloc(unsigned long size)
{
    return task_heap_alloc(size ? size : TASK_PAGE_SIZE,
                           TASK_PAGE_SIZE, 1);
}

static void task_page_free(void *ptr, unsigned long size)
{
    (void)size;
    (void)task_heap_free(ptr);
}

static void *task_mmap(void *addr, unsigned long size, enum lkl_prot prot)
{
    (void)addr;
    (void)prot;
    return task_heap_alloc(size, TASK_PAGE_SIZE, 1);
}

static int task_munmap(void *addr, unsigned long size)
{
    (void)size;
    return task_heap_free(addr);
}

static void task_shmem_init(unsigned long size) { (void)size; }
static void *task_shmem_mmap(void *addr, unsigned long page_offset,
                            unsigned long size, enum lkl_prot prot)
{
    (void)addr;
    (void)page_offset;
    (void)prot;
    return task_heap_alloc(size, TASK_PAGE_SIZE, 1);
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
static volatile int task_timer_allocated;
static volatile int task_timer_callback_running;
static volatile int task_timer_lock_word;
static volatile unsigned long long task_timer_generation;
static volatile unsigned long long task_timer_armed_generation;
static unsigned long long task_timer_deadline_ns;

static void task_timer_lock(void)
{
    while (__atomic_test_and_set(&task_timer_lock_word, __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void task_timer_unlock(void)
{
    __atomic_clear(&task_timer_lock_word, __ATOMIC_RELEASE);
}

static void task_timer_service(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned long long generation = __atomic_load_n(
            &task_timer_armed_generation, __ATOMIC_ACQUIRE);
        if (generation) {
            unsigned long long deadline =
                __atomic_load_n(&task_timer_deadline_ns, __ATOMIC_RELAXED);
            if (task_time() >= deadline) {
                unsigned long long expected = generation;
                if (__atomic_compare_exchange_n(
                        &task_timer_armed_generation, &expected, 0, false,
                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    __atomic_store_n(&task_timer_callback_running, 1,
                                     __ATOMIC_RELEASE);
                    if (__atomic_load_n(&task_timer_generation,
                                        __ATOMIC_ACQUIRE) == generation &&
                        __atomic_load_n(&task_timer_allocated,
                                        __ATOMIC_ACQUIRE)) {
                        void (*fn)(void) = task_timer_fn;
                        if (fn) fn();
                    }
                    __atomic_store_n(&task_timer_callback_running, 0,
                                     __ATOMIC_RELEASE);
                }
            }
        }
        seL4_Yield();
    }
}

static void *task_timer_alloc(void (*fn)(void))
{
    if (!fn || !task_tsc_frequency) return NULL;
    task_timer_lock();
    if (task_timer_allocated) {
        task_timer_unlock();
        return NULL;
    }
    task_timer_fn = fn;
    __atomic_add_fetch(&task_timer_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&task_timer_armed_generation, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&task_timer_allocated, 1, __ATOMIC_RELEASE);
    if (!task_timer_started) {
        struct task_thread_slot *slot = &task_threads[LUNA_TIMER_SLOT];
        if (__atomic_load_n(&slot->used, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&task_timer_allocated, 0, __ATOMIC_RELEASE);
            task_timer_fn = NULL;
            task_timer_unlock();
            return NULL;
        }
        __atomic_store_n(&slot->used, 1, __ATOMIC_RELEASE);
        slot->tid = TASK_TIMER_TID;
        slot->exited = 0;
        slot->join_claimed = 0;
        slot->fn = task_timer_service;
        slot->arg = NULL;
        if (sel4utils_start_thread(&slot->thread, task_thread_trampoline,
                                   slot, NULL, 1)) {
            __atomic_store_n(&slot->used, 0, __ATOMIC_RELEASE);
            slot->tid = 0;
            slot->fn = NULL;
            slot->arg = NULL;
            __atomic_store_n(&task_timer_allocated, 0, __ATOMIC_RELEASE);
            task_timer_fn = NULL;
            task_timer_unlock();
            return NULL;
        }
        task_timer_started = 1;
    }
    task_timer_unlock();
    return &task_timer_handle;
}

static int task_timer_set_oneshot(void *timer, unsigned long ns)
{
    task_timer_lock();
    if (timer != &task_timer_handle || !task_timer_started ||
        !task_timer_allocated) {
        task_timer_unlock();
        return -1;
    }
    unsigned long long generation = __atomic_add_fetch(
        &task_timer_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&task_timer_deadline_ns,
                     task_time() + (ns ? ns : 1), __ATOMIC_RELAXED);
    __atomic_store_n(&task_timer_armed_generation, generation,
                     __ATOMIC_RELEASE);
    task_timer_unlock();
    return 0;
}

static void task_timer_free(void *timer)
{
    task_timer_lock();
    if (timer != &task_timer_handle || !task_timer_allocated) {
        task_timer_unlock();
        return;
    }
    __atomic_store_n(&task_timer_allocated, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&task_timer_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&task_timer_armed_generation, 0, __ATOMIC_RELEASE);
    task_timer_fn = NULL;
    task_timer_unlock();
    while (__atomic_load_n(&task_timer_callback_running,
                           __ATOMIC_ACQUIRE))
        seL4_Yield();
}

#define TASK_CONSOLE_TID 0x7ffffffeUL
#define TASK_CONSOLE_RING 256
static seL4_CPtr task_console_io_port;
static unsigned char task_console_ring[TASK_CONSOLE_RING];
static volatile unsigned task_console_head;
static volatile unsigned task_console_tail;
static volatile int task_console_started;
static volatile int task_console_stop_requested;
static int task_console_irq;

static void task_console_service(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&task_console_stop_requested, __ATOMIC_ACQUIRE)) {
        seL4_X86_IOPort_In8_t lsr =
            seL4_X86_IOPort_In8(task_console_io_port, 0x3fd);
        if (lsr.error == seL4_NoError && (lsr.result & 0x01)) {
            seL4_X86_IOPort_In8_t input =
                seL4_X86_IOPort_In8(task_console_io_port, 0x3f8);
            if (input.error == seL4_NoError) {
                unsigned next =
                    (task_console_head + 1) % TASK_CONSOLE_RING;
                if (next != task_console_tail) {
                    int was_empty = task_console_head == task_console_tail;
                    task_console_ring[task_console_head] = input.result;
                    task_console_head = next;
                    if (was_empty && task_console_irq)
                        lkl_trigger_irq(task_console_irq);
                }
            }
        }
        seL4_Yield();
    }
}

static void task_console_start(int irq)
{
    if (!task_console_io_port || task_console_started || irq <= 0) return;
    struct task_thread_slot *slot = &task_threads[LUNA_CONSOLE_SLOT];
    if (__atomic_load_n(&slot->used, __ATOMIC_ACQUIRE)) return;
    task_console_irq = irq;
    task_console_head = 0;
    task_console_tail = 0;
    __atomic_store_n(&task_console_stop_requested, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&slot->used, 1, __ATOMIC_RELEASE);
    slot->tid = TASK_CONSOLE_TID;
    slot->exited = 0;
    slot->join_claimed = 0;
    slot->fn = task_console_service;
    slot->arg = NULL;
    if (sel4utils_start_thread(&slot->thread, task_thread_trampoline,
                               slot, NULL, 1)) {
        __atomic_store_n(&slot->used, 0, __ATOMIC_RELEASE);
        slot->tid = 0;
        slot->fn = NULL;
        task_console_irq = 0;
        return;
    }
    task_console_started = 1;
}

static int task_console_take(void)
{
    if (task_console_head == task_console_tail) return -1;
    unsigned char value = task_console_ring[task_console_tail];
    task_console_tail = (task_console_tail + 1) % TASK_CONSOLE_RING;
    return value;
}

int luna_lkl_task_console_ready(void) { return task_console_started; }

void luna_lkl_task_console_stop(void)
{
    if (!task_console_started) return;
    __atomic_store_n(&task_console_stop_requested, 1, __ATOMIC_RELEASE);
    struct task_thread_slot *slot = &task_threads[LUNA_CONSOLE_SLOT];
    seL4_Word badge = 0;
    if (!__atomic_load_n(&slot->exited, __ATOMIC_ACQUIRE))
        seL4_Wait(slot->join_ntfn, &badge);
    else
        seL4_Poll(slot->join_ntfn, &badge);
    __atomic_store_n(&slot->used, 0, __ATOMIC_RELEASE);
    slot->tid = 0;
    slot->exited = 0;
    slot->join_claimed = 0;
    slot->fn = NULL;
    slot->arg = NULL;
    task_console_started = 0;
    task_console_irq = 0;
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

struct lkl_host_operations lkl_host_ops = {
    .virtio_devices = lkl_virtio_devs,
    .print = task_print,
    .panic = task_panic,
    .console_start = task_console_start,
    .console_take = task_console_take,
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
    .ioremap = lkl_ioremap,
    .iomem_access = lkl_iomem_access,
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
    const struct luna_task_sync_resource sync[LUNA_SYNC_SLOTS],
    seL4_CPtr console_io_port, seL4_CPtr control_ep,
    seL4_CPtr command_ep)
{
    memset(task_threads, 0, sizeof(task_threads));
    current_tid = 1;
    current_tls_index = 1;
    task_next_tid = 2;
    task_thread_table_lock_word = 0;
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct task_thread_slot *slot = &task_threads[i];
        slot->self_tcb = resources[i].tcb;
        slot->join_ntfn = resources[i].join_ntfn;
        slot->thread.tcb.cptr = resources[i].tcb;
        slot->thread.stack_top = (void *)(uintptr_t)resources[i].stack_top;
        slot->thread.initial_stack_pointer = slot->thread.stack_top;
        slot->thread.stack_size = resources[i].stack_pages;
        slot->thread.ipc_buffer_addr = resources[i].ipc_buffer_addr;
        slot->tls_index = (unsigned)i + 2;
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
    task_console_io_port = console_io_port;
    task_manager_control_ep = control_ep;
    task_manager_command_ep = command_ep;
    if (!task_console_io_port || !task_manager_control_ep ||
        !task_manager_command_ep)
        return -1;
    task_console_head = 0;
    task_console_tail = 0;
    task_console_started = 0;
    task_console_stop_requested = 0;
    task_console_irq = 0;
    task_timer_fn = NULL;
    task_timer_started = 0;
    task_timer_allocated = 0;
    task_timer_callback_running = 0;
    task_timer_lock_word = 0;
    task_timer_generation = 0;
    task_timer_armed_generation = 0;
    task_timer_deadline_ns = 0;
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

#define TASK_SEM_TEST_WAITERS 4
#define TASK_MUTEX_TEST_WORKERS 4
#define TASK_MUTEX_TEST_ITERATIONS 32

static struct lkl_sem *task_test_sem;
static volatile int task_test_sem_ready;
static volatile int task_test_sem_passed;

static void task_sem_test_worker(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&task_test_sem_ready, 1, __ATOMIC_RELEASE);
    task_sem_down(task_test_sem);
    __atomic_fetch_add(&task_test_sem_passed, 1, __ATOMIC_RELEASE);
}

static struct lkl_mutex *task_test_mutex;
static volatile int task_test_mutex_owner_rejected;
static volatile int task_test_mutex_value;

static void task_mutex_owner_test_worker(void *arg)
{
    (void)arg;
    task_test_mutex_owner_rejected =
        task_mutex_unlock_checked(task_test_mutex) == -1;
}

static void task_mutex_contention_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < TASK_MUTEX_TEST_ITERATIONS; i++) {
        task_mutex_lock(task_test_mutex);
        int value = task_test_mutex_value;
        seL4_Yield();
        task_test_mutex_value = value + 1;
        task_mutex_unlock(task_test_mutex);
    }
}

static struct lkl_tls_key *task_test_tls_key;
static volatile int task_test_tls_destructor_calls;
static volatile int task_test_tls_set_errors;
static int task_test_tls_rearm_limit;

static void task_tls_reentrant_destructor(void *data)
{
    (void)data;
    int call = __atomic_add_fetch(&task_test_tls_destructor_calls, 1,
                                  __ATOMIC_ACQ_REL);
    if (call < task_test_tls_rearm_limit &&
        task_tls_set(task_test_tls_key, (void *)(uintptr_t)(call + 1)))
        __atomic_fetch_add(&task_test_tls_set_errors, 1,
                           __ATOMIC_RELAXED);
}

static void task_tls_test_worker(void *arg)
{
    if (task_tls_set(task_test_tls_key, arg))
        __atomic_fetch_add(&task_test_tls_set_errors, 1,
                           __ATOMIC_RELAXED);
}

static int task_tls_reentry_test(int rearm_limit, int expected_calls)
{
    task_test_tls_destructor_calls = 0;
    task_test_tls_set_errors = 0;
    task_test_tls_rearm_limit = rearm_limit;
    task_test_tls_key = task_tls_alloc(task_tls_reentrant_destructor);
    if (!task_test_tls_key) return -1;

    lkl_thread_t tid = task_thread_create(task_tls_test_worker,
                                          (void *)(uintptr_t)1);
    struct task_thread_slot *slot = tid ? thread_by_tid(tid) : NULL;
    unsigned tls_index = slot ? slot->tls_index : 0;
    if (!tid || !slot || task_thread_join(tid) ||
        task_test_tls_destructor_calls != expected_calls ||
        task_test_tls_set_errors != 0 ||
        task_test_tls_key->data[tls_index] != NULL) {
        task_tls_free(task_test_tls_key);
        task_test_tls_key = NULL;
        return -1;
    }
    task_tls_free(task_test_tls_key);
    task_test_tls_key = NULL;
    return task_test_tls_destructor_calls == expected_calls ? 0 : -1;
}

int luna_lkl_task_sync_tls_test(void)
{
    struct lkl_sem *counting = task_sem_alloc(3);
    if (!counting) return -1;
    task_sem_down(counting);
    task_sem_down(counting);
    task_sem_down(counting);
    if (__atomic_load_n(&counting->count, __ATOMIC_ACQUIRE) != 0) return -1;
    task_sem_up(counting);
    task_sem_up(counting);
    task_sem_down(counting);
    task_sem_down(counting);
    if (__atomic_load_n(&counting->count, __ATOMIC_ACQUIRE) != 0) return -1;
    task_sem_free(counting);

    task_test_sem = task_sem_alloc(0);
    if (!task_test_sem) return -1;
    task_test_sem_ready = 0;
    task_test_sem_passed = 0;
    lkl_thread_t sem_tids[TASK_SEM_TEST_WAITERS];
    for (int i = 0; i < TASK_SEM_TEST_WAITERS; i++) {
        sem_tids[i] = task_thread_create(task_sem_test_worker, NULL);
        if (!sem_tids[i]) return -1;
    }
    while (__atomic_load_n(&task_test_sem_ready, __ATOMIC_ACQUIRE) !=
               TASK_SEM_TEST_WAITERS ||
           __atomic_load_n(&task_test_sem->waiters, __ATOMIC_ACQUIRE) !=
               TASK_SEM_TEST_WAITERS)
        seL4_Yield();
    for (int i = 0; i < TASK_SEM_TEST_WAITERS; i++)
        task_sem_up(task_test_sem);
    for (int i = 0; i < TASK_SEM_TEST_WAITERS; i++)
        if (task_thread_join(sem_tids[i])) return -1;
    if (task_test_sem_passed != TASK_SEM_TEST_WAITERS ||
        task_test_sem->count != 0 || task_test_sem->waiters != 0)
        return -1;
    task_sem_free(task_test_sem);
    task_test_sem = NULL;

    task_test_mutex = task_mutex_alloc(0);
    if (!task_test_mutex) return -1;
    task_mutex_lock(task_test_mutex);
    unsigned owner_errors = __atomic_load_n(&task_mutex_owner_errors,
                                            __ATOMIC_ACQUIRE);
    task_test_mutex_owner_rejected = 0;
    lkl_thread_t owner_tid = task_thread_create(
        task_mutex_owner_test_worker, NULL);
    if (!owner_tid || task_thread_join(owner_tid) ||
        !task_test_mutex_owner_rejected || task_test_mutex->owner != 1 ||
        task_test_mutex->count != 1 ||
        task_mutex_owner_errors != owner_errors + 1 ||
        task_mutex_unlock_checked(task_test_mutex))
        return -1;

    task_test_mutex_value = 0;
    lkl_thread_t mutex_tids[TASK_MUTEX_TEST_WORKERS];
    for (int i = 0; i < TASK_MUTEX_TEST_WORKERS; i++) {
        mutex_tids[i] = task_thread_create(task_mutex_contention_worker, NULL);
        if (!mutex_tids[i]) return -1;
    }
    for (int i = 0; i < TASK_MUTEX_TEST_WORKERS; i++)
        if (task_thread_join(mutex_tids[i])) return -1;
    if (task_test_mutex_value !=
        TASK_MUTEX_TEST_WORKERS * TASK_MUTEX_TEST_ITERATIONS)
        return -1;
    task_mutex_free(task_test_mutex);

    task_test_mutex = task_mutex_alloc(1);
    if (!task_test_mutex) return -1;
    task_mutex_lock(task_test_mutex);
    task_mutex_lock(task_test_mutex);
    if (task_test_mutex->owner != 1 || task_test_mutex->count != 2 ||
        task_mutex_unlock_checked(task_test_mutex) ||
        task_test_mutex->owner != 1 || task_test_mutex->count != 1 ||
        task_mutex_unlock_checked(task_test_mutex) ||
        task_test_mutex->owner != 0 || task_test_mutex->count != 0)
        return -1;
    task_mutex_free(task_test_mutex);
    task_test_mutex = NULL;

    if (task_tls_reentry_test(3, 3) ||
        task_tls_reentry_test(TASK_TLS_DESTRUCTOR_ITERATIONS + 2,
                              TASK_TLS_DESTRUCTOR_ITERATIONS))
        return -1;
    return 0;
}

#define TASK_THREAD_REUSE_ROUNDS 64
#define TASK_TIMER_REARM_NS      8000000UL

static struct lkl_sem *task_join_test_gate;
static lkl_thread_t task_join_test_target;
static volatile int task_join_test_ready;
static volatile int task_join_test_results[2];

static void task_join_target_worker(void *arg)
{
    (void)arg;
    task_sem_down(task_join_test_gate);
}

static void task_joiner_worker(void *arg)
{
    int index = (int)(uintptr_t)arg;
    __atomic_fetch_add(&task_join_test_ready, 1, __ATOMIC_RELEASE);
    task_join_test_results[index] = task_thread_join(task_join_test_target);
}

static void task_noop_worker(void *arg)
{
    (void)arg;
}

static void task_explicit_exit_worker(void *arg)
{
    (void)arg;
    task_thread_exit();
}

static int task_thread_lifecycle_test(void)
{
    task_join_test_gate = task_sem_alloc(0);
    if (!task_join_test_gate) return -1;
    task_join_test_ready = 0;
    task_join_test_results[0] = 99;
    task_join_test_results[1] = 99;
    task_join_test_target = task_thread_create(task_join_target_worker, NULL);
    if (!task_join_test_target) return -1;

    lkl_thread_t joiners[2];
    for (int i = 0; i < 2; i++) {
        joiners[i] = task_thread_create(task_joiner_worker,
                                        (void *)(uintptr_t)i);
        if (!joiners[i]) return -1;
    }
    while (__atomic_load_n(&task_join_test_ready, __ATOMIC_ACQUIRE) != 2)
        seL4_Yield();
    task_sem_up(task_join_test_gate);
    if (task_thread_join(joiners[0]) || task_thread_join(joiners[1]))
        return -1;
    int successes = (task_join_test_results[0] == 0) +
                    (task_join_test_results[1] == 0);
    int rejected = (task_join_test_results[0] == -1) +
                   (task_join_test_results[1] == -1);
    if (successes != 1 || rejected != 1 ||
        task_thread_join(task_join_test_target) != -1)
        return -1;
    task_sem_free(task_join_test_gate);
    task_join_test_gate = NULL;

    lkl_thread_t replacement = task_thread_create(task_noop_worker, NULL);
    if (!replacement || replacement == task_join_test_target ||
        task_thread_join(task_join_test_target) != -1 ||
        task_thread_join(replacement) || task_thread_join(replacement) != -1)
        return -1;

    lkl_thread_t exiting = task_thread_create(task_explicit_exit_worker, NULL);
    if (!exiting || task_thread_join(exiting)) return -1;

    lkl_thread_t previous = 0;
    for (int i = 0; i < TASK_THREAD_REUSE_ROUNDS; i++) {
        lkl_thread_t tid = task_thread_create(task_noop_worker, NULL);
        if (!tid || tid == previous || task_thread_equal(tid, previous) ||
            task_thread_join(tid))
            return -1;
        previous = tid;
    }
    return task_thread_join((lkl_thread_t)-1) == -1 ? 0 : -1;
}

static volatile int task_timer_test_callbacks;

static void task_timer_test_callback(void)
{
    __atomic_fetch_add(&task_timer_test_callbacks, 1, __ATOMIC_ACQ_REL);
}

static void task_timer_test_wait(unsigned long long ns)
{
    unsigned long long deadline = task_time() + ns;
    while (task_time() < deadline) seL4_Yield();
}

static int task_timer_lifecycle_test(void)
{
    task_timer_test_callbacks = 0;
    void *timer = task_timer_alloc(task_timer_test_callback);
    if (!timer || task_timer_set_oneshot(timer, 2000000UL)) return -1;
    task_timer_free(timer);
    task_timer_test_wait(4000000ULL);
    if (task_timer_test_callbacks != 0) return -1;

    timer = task_timer_alloc(task_timer_test_callback);
    if (!timer || task_timer_set_oneshot(timer, 1000000UL)) return -1;
    for (int i = 0; i < 32; i++)
        if (task_timer_set_oneshot(timer, TASK_TIMER_REARM_NS)) return -1;
    task_timer_test_wait(3000000ULL);
    if (task_timer_test_callbacks != 0) return -1;
    task_timer_test_wait(10000000ULL);
    if (task_timer_test_callbacks != 1) return -1;
    task_timer_free(timer);

    for (int i = 0; i < 16; i++) {
        timer = task_timer_alloc(task_timer_test_callback);
        if (!timer || task_timer_set_oneshot(timer, 100000UL)) return -1;
        seL4_Yield();
        task_timer_free(timer);
        int callbacks = __atomic_load_n(&task_timer_test_callbacks,
                                        __ATOMIC_ACQUIRE);
        task_timer_test_wait(300000ULL);
        if (__atomic_load_n(&task_timer_test_callbacks,
                            __ATOMIC_ACQUIRE) != callbacks)
            return -1;
    }
    return 0;
}

int luna_lkl_task_thread_timer_test(void)
{
    if (task_thread_lifecycle_test() || task_timer_lifecycle_test())
        return -1;
    return 0;
}

int luna_lkl_task_sync_tls_runtime_ok(void)
{
    if (__atomic_load_n(&task_mutex_owner_errors, __ATOMIC_ACQUIRE) != 0) {
        task_debug("luna-lkl-task: mutex owner errors during LKL runtime\n");
        return 0;
    }
    return 1;
}

int luna_lkl_task_allocator_test(void)
{
    task_heap_init();

    void *small_a = task_mem_alloc(73);
    void *page_a = task_page_alloc(2 * TASK_PAGE_SIZE);
    void *small_b = task_mem_alloc(257);
    if (!small_a || !page_a || !small_b ||
        ((uintptr_t)small_a & (TASK_HEAP_ALIGNMENT - 1)) ||
        ((uintptr_t)page_a & (TASK_PAGE_SIZE - 1)))
        return -1;

    memset(page_a, 0xa5, 2 * TASK_PAGE_SIZE);
    task_page_free(page_a, 2 * TASK_PAGE_SIZE);
    void *page_b = task_page_alloc(TASK_PAGE_SIZE);
    if (page_b != page_a || ((unsigned char *)page_b)[0] != 0 ||
        ((unsigned char *)page_b)[TASK_PAGE_SIZE - 1] != 0)
        return -1;

    task_mem_free(small_a);
    task_mem_free(small_b);
    task_page_free(page_b, TASK_PAGE_SIZE);
    if (!task_heap_is_idle()) return -1;

    void *mapped = task_mmap(NULL, 3 * TASK_PAGE_SIZE,
                             LKL_PROT_READ | LKL_PROT_WRITE);
    if (!mapped || task_munmap(mapped, 3 * TASK_PAGE_SIZE)) return -1;
    void *shared = task_shmem_mmap(NULL, 0, 2 * TASK_PAGE_SIZE,
                                   LKL_PROT_READ | LKL_PROT_WRITE);
    if (!shared || task_munmap(shared, 2 * TASK_PAGE_SIZE)) return -1;

    /* Touch the whole reserved arena once. Besides testing maximum-capacity
     * allocation, this materializes per-page vspace metadata so reservation
     * teardown can verify every page after it has been unmapped. */
    void *large = task_page_alloc(LUNA_CHILD_HEAP_SIZE);
    if (!large) return -1;
    task_page_free(large, LUNA_CHILD_HEAP_SIZE);
    if (!task_heap_is_idle() || task_heap_peak_bytes < LUNA_CHILD_HEAP_SIZE)
        return -1;

    task_heap_init();
    return 0;
}

int luna_lkl_task_allocator_idle(void)
{
    if (!task_heap_is_idle()) {
        task_debug("luna-lkl-task: host heap not fully released after halt\n");
        return 0;
    }
    return 1;
}

int luna_lkl_task_init(void)
{
    task_heap_init();
    task_mutex_owner_errors = 0;
    memset(task_tls, 0, sizeof(task_tls));
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        task_sync_drain(task_sync[i].ntfn);
        task_sync[i].used = 0;
        task_sync[i].kind = TASK_SYNC_FREE;
    }
    task_time_epoch = __builtin_ia32_rdtsc();
    return lkl_init(&lkl_host_ops);
}

int luna_lkl_task_prepare_time(unsigned long long tsc_frequency)
{
    if (!tsc_frequency) return -1;
    task_tsc_frequency = tsc_frequency;
    task_time_epoch = __builtin_ia32_rdtsc();
    return 0;
}

int luna_lkl_task_start_kernel(unsigned long long tsc_frequency)
{
    if (luna_lkl_task_prepare_time(tsc_frequency)) return -1;
    return lkl_start_kernel("mem=16M loglevel=4");
}

long luna_lkl_task_halt(void)
{
    return lkl_sys_halt();
}

unsigned long long luna_lkl_task_time(void)
{
    return task_time();
}
