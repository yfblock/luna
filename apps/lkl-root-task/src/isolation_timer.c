/* SPDX-License-Identifier: GPL-2.0 */
/* Root-owned PIT one-shot service. The child receives only wake notifications;
 * timer I/O ports and IRQ authority remain in the manager. */
#include "luna_timer_manager.h"

#include <platsupport/plat/pit.h>
#include <sel4platsupport/io.h>
#include <sel4utils/thread.h>
#include <utils/util.h>
#include <vka/object_capops.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The root manager also takes timer_state.lock at seL4_MaxPrio. */
#define LUNA_TIMER_PRIORITY seL4_MaxPrio
#define LUNA_TIMER_STACK_PAGES 2U

struct luna_timer_state {
    ps_io_ops_t io_ops;
    pit_t pit;
    vka_object_t irq_ntfn;
    sel4utils_thread_t irq_thread;
    cspacepath_t irq_handler_path;
    volatile int lock;
    volatile int ready;
    volatile int armed;
    volatile seL4_CPtr child_ntfn;
    volatile unsigned long long generation;
    volatile unsigned long long deadline_cycles;
    volatile int network_armed;
    volatile seL4_CPtr network_ntfn;
    volatile unsigned long long network_deadline_cycles;
    unsigned long long tsc_frequency;
    volatile unsigned long long arm_count;
    volatile unsigned long long cancel_count;
    volatile unsigned long long interrupt_count;
    volatile unsigned long long wake_count;
    volatile unsigned long long rearm_count;
    volatile unsigned long long network_arm_count;
    volatile unsigned long long network_cancel_count;
    volatile unsigned long long network_wake_count;
};

/* Keep mutable IRQ state away from the root fixed-pool/BSS boundary. */
static struct luna_timer_state *timer_state_ptr =
    (struct luna_timer_state *)(uintptr_t)1;
#define timer_state (*timer_state_ptr)

static void timer_lock(void)
{
    while (__atomic_test_and_set(&timer_state.lock, __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void timer_unlock(void)
{
    __atomic_clear(&timer_state.lock, __ATOMIC_RELEASE);
}

static unsigned long long ns_to_cycles(unsigned long long ns)
{
    unsigned long long seconds = ns / 1000000000ULL;
    unsigned long long remainder = ns % 1000000000ULL;
    return seconds * timer_state.tsc_frequency +
           (remainder * timer_state.tsc_frequency + 999999999ULL) /
               1000000000ULL;
}

static unsigned long long cycles_to_ns(unsigned long long cycles)
{
    unsigned long long seconds = cycles / timer_state.tsc_frequency;
    unsigned long long remainder = cycles % timer_state.tsc_frequency;
    return seconds * 1000000000ULL +
           (remainder * 1000000000ULL) / timer_state.tsc_frequency;
}

static int program_next(unsigned long long now)
{
    unsigned long long deadline = 0;
    if (__atomic_load_n(&timer_state.armed, __ATOMIC_ACQUIRE))
        deadline = __atomic_load_n(&timer_state.deadline_cycles,
                                   __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE)) {
        unsigned long long network_deadline = __atomic_load_n(
            &timer_state.network_deadline_cycles, __ATOMIC_ACQUIRE);
        if (!deadline || network_deadline < deadline)
            deadline = network_deadline;
    }
    if (!deadline) return pit_cancel_timeout(&timer_state.pit);
    unsigned long long remaining_cycles = deadline > now ? deadline - now : 0;
    unsigned long long remaining_ns = cycles_to_ns(remaining_cycles);
    if (remaining_ns < PIT_MIN_NS) remaining_ns = PIT_MIN_NS;
    if (remaining_ns > PIT_MAX_NS) remaining_ns = PIT_MAX_NS;
    return pit_set_timeout(&timer_state.pit, remaining_ns, false);
}

static void handle_timer_irq(void)
{
    seL4_CPtr child_ntfn = seL4_CapNull;
    seL4_CPtr network_ntfn = seL4_CapNull;
    timer_lock();
    __atomic_fetch_add(&timer_state.interrupt_count, 1ULL,
                       __ATOMIC_RELAXED);
    if (__atomic_load_n(&timer_state.armed, __ATOMIC_ACQUIRE)) {
        unsigned long long now = __builtin_ia32_rdtsc();
        unsigned long long deadline = __atomic_load_n(
            &timer_state.deadline_cycles, __ATOMIC_ACQUIRE);
        if (now >= deadline) {
            __atomic_store_n(&timer_state.armed, 0, __ATOMIC_RELEASE);
            child_ntfn = __atomic_load_n(&timer_state.child_ntfn,
                                         __ATOMIC_ACQUIRE);
        }
    }
    unsigned long long now = __builtin_ia32_rdtsc();
    if (__atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE) &&
        now >= __atomic_load_n(&timer_state.network_deadline_cycles,
                               __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&timer_state.network_armed, 0, __ATOMIC_RELEASE);
        network_ntfn = __atomic_load_n(&timer_state.network_ntfn,
                                       __ATOMIC_ACQUIRE);
    }
    if (__atomic_load_n(&timer_state.armed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE)) {
        if (!program_next(now))
            __atomic_fetch_add(&timer_state.rearm_count, 1ULL,
                               __ATOMIC_RELAXED);
        else {
            __atomic_store_n(&timer_state.armed, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&timer_state.network_armed, 0,
                             __ATOMIC_RELEASE);
        }
    }
    timer_unlock();
    if (child_ntfn) {
        seL4_Signal(child_ntfn);
        __atomic_fetch_add(&timer_state.wake_count, 1ULL,
                           __ATOMIC_RELAXED);
    }
    if (network_ntfn) {
        seL4_Signal(network_ntfn);
        __atomic_fetch_add(&timer_state.network_wake_count, 1ULL,
                           __ATOMIC_RELAXED);
    }
}

static void timer_irq_thread(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg0;
    (void)arg1;
    (void)ipc_buffer;
    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(timer_state.irq_ntfn.cptr, &badge);
        handle_timer_irq();
        if (seL4_IRQHandler_Ack(timer_state.irq_handler_path.capPtr) !=
            seL4_NoError) {
            __atomic_store_n(&timer_state.ready, 0, __ATOMIC_RELEASE);
            seL4_TCB_Suspend(timer_state.irq_thread.tcb.cptr);
            for (;;) seL4_Yield();
        }
    }
}

static void cleanup_irq(vka_t *vka, vspace_t *manager_vspace)
{
    if (timer_state.irq_thread.tcb.cptr) {
        sel4utils_clean_up_thread(vka, manager_vspace,
                                  &timer_state.irq_thread);
        memset(&timer_state.irq_thread, 0, sizeof(timer_state.irq_thread));
    }
    if (timer_state.irq_handler_path.capPtr) {
        if (seL4_IRQHandler_Clear(timer_state.irq_handler_path.capPtr) !=
            seL4_NoError)
            printf("luna: timer IRQ clear failed\n");
        vka_cnode_delete(&timer_state.irq_handler_path);
        vka_cspace_free_path(vka, timer_state.irq_handler_path);
        memset(&timer_state.irq_handler_path, 0,
               sizeof(timer_state.irq_handler_path));
    }
    if (timer_state.irq_ntfn.cptr) {
        vka_free_object(vka, &timer_state.irq_ntfn);
        memset(&timer_state.irq_ntfn, 0, sizeof(timer_state.irq_ntfn));
    }
}

int luna_timer_manager_init(simple_t *simple, vka_t *vka,
                            vspace_t *manager_vspace,
                            unsigned long long tsc_frequency)
{
    timer_state_ptr = calloc(1, sizeof(*timer_state_ptr));
    if (!timer_state_ptr) {
        printf("luna: manager one-shot timer state allocation failed\n");
        return -1;
    }
    timer_state.tsc_frequency = tsc_frequency;
    if (!tsc_frequency ||
        sel4platsupport_new_io_ops(manager_vspace, vka, simple,
                                   &timer_state.io_ops) ||
        sel4platsupport_new_arch_ops(&timer_state.io_ops, simple, vka) ||
        pit_init(&timer_state.pit, timer_state.io_ops.io_port_ops) ||
        vka_alloc_notification(vka, &timer_state.irq_ntfn)) {
        printf("luna: manager one-shot timer setup failed\n");
        cleanup_irq(vka, manager_vspace);
        free(timer_state_ptr);
        timer_state_ptr = (struct luna_timer_state *)(uintptr_t)1;
        return -1;
    }
    if (vka_cspace_alloc_path(vka, &timer_state.irq_handler_path))
        goto fail;
#ifdef CONFIG_IRQ_PIC
    if (simple_get_IRQ_handler(simple, PIT_INTERRUPT,
                               timer_state.irq_handler_path) != seL4_NoError)
        goto fail;
#else
    if (arch_simple_get_ioapic(&simple->arch_simple,
                               timer_state.irq_handler_path, 0,
                               PIT_INTERRUPT, 0, 0,
                               PIT_INTERRUPT) != seL4_NoError)
        goto fail;
#endif
    if (seL4_IRQHandler_SetNotification(
            timer_state.irq_handler_path.capPtr,
            timer_state.irq_ntfn.cptr) != seL4_NoError ||
        seL4_IRQHandler_Ack(timer_state.irq_handler_path.capPtr) !=
            seL4_NoError)
        goto fail;
    sel4utils_thread_config_t config = thread_config_new(simple);
    config = thread_config_priority(config, LUNA_TIMER_PRIORITY);
    config = thread_config_stack_size(config, LUNA_TIMER_STACK_PAGES);
    config = thread_config_create_reply(config);
    if (sel4utils_configure_thread_config(vka, manager_vspace,
                                          manager_vspace, config,
                                          &timer_state.irq_thread) ||
        sel4utils_start_thread(&timer_state.irq_thread, timer_irq_thread,
                               NULL, NULL, 1))
        goto fail;
    NAME_THREAD(timer_state.irq_thread.tcb.cptr, "luna-timer-irq");
    __atomic_store_n(&timer_state.ready, 1, __ATOMIC_RELEASE);
    printf("LUNA_TIMER_MANAGER_READY mechanism=pit-notification\n");
    return 0;

fail:
    cleanup_irq(vka, manager_vspace);
    free(timer_state_ptr);
    timer_state_ptr = (struct luna_timer_state *)(uintptr_t)1;
    printf("luna: manager one-shot timer IRQ setup failed\n");
    return -1;
}

void luna_timer_manager_deactivate(void)
{
    if (!__atomic_load_n(&timer_state.ready, __ATOMIC_ACQUIRE)) return;
    timer_lock();
    __atomic_store_n(&timer_state.armed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.child_ntfn, seL4_CapNull,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.generation, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.deadline_cycles, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.network_armed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.network_ntfn, seL4_CapNull,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&timer_state.network_deadline_cycles, 0,
                     __ATOMIC_RELEASE);
    (void)pit_cancel_timeout(&timer_state.pit);
    timer_unlock();
}

void luna_timer_manager_destroy(vka_t *vka, vspace_t *manager_vspace)
{
    if (!timer_state_ptr || timer_state_ptr == (void *)(uintptr_t)1) return;
    luna_timer_manager_deactivate();
    __atomic_store_n(&timer_state.ready, 0, __ATOMIC_RELEASE);
    cleanup_irq(vka, manager_vspace);
    free(timer_state_ptr);
    timer_state_ptr = (struct luna_timer_state *)(uintptr_t)1;
}

int luna_timer_manager_service(seL4_CPtr command_ep, seL4_CPtr child_ntfn,
                               seL4_Word event, seL4_Word generation,
                               seL4_Word timeout_ns)
{
    int error = -1;
    if (__atomic_load_n(&timer_state.ready, __ATOMIC_ACQUIRE) &&
        child_ntfn && generation) {
        timer_lock();
        seL4_CPtr current_child = __atomic_load_n(
            &timer_state.child_ntfn, __ATOMIC_ACQUIRE);
        unsigned long long current_generation = __atomic_load_n(
            &timer_state.generation, __ATOMIC_ACQUIRE);
        if (event == LUNA_ISOLATION_EVENT_TIMER_ARM && timeout_ns) {
            if (current_child && current_child != child_ntfn) {
                error = -1;
            } else if ((unsigned long long)generation <
                       current_generation) {
                /* A service-thread re-arm raced with a newer set/cancel.
                 * The newer generation already owns the hardware state. */
                error = 0;
            } else {
                unsigned long long cycles = ns_to_cycles(timeout_ns);
                if (!cycles) cycles = 1;
                __atomic_store_n(&timer_state.child_ntfn, child_ntfn,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&timer_state.generation, generation,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&timer_state.deadline_cycles,
                                 __builtin_ia32_rdtsc() + cycles,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&timer_state.armed, 1, __ATOMIC_RELEASE);
                error = program_next(__builtin_ia32_rdtsc());
                if (error)
                    __atomic_store_n(&timer_state.armed, 0,
                                     __ATOMIC_RELEASE);
                else
                    __atomic_fetch_add(&timer_state.arm_count, 1ULL,
                                       __ATOMIC_RELAXED);
            }
        } else if (event == LUNA_ISOLATION_EVENT_TIMER_CANCEL &&
                   !timeout_ns) {
            if (current_child && current_child != child_ntfn) {
                error = -1;
            } else if ((unsigned long long)generation <
                       current_generation) {
                error = 0;
            } else {
                /* Advancing generation before cancelling prevents an older
                 * in-flight ARM from reviving a freed timer. */
                __atomic_store_n(&timer_state.generation, generation,
                                 __ATOMIC_RELEASE);
                __atomic_store_n(&timer_state.armed, 0, __ATOMIC_RELEASE);
                error = program_next(__builtin_ia32_rdtsc());
                if (!error)
                    __atomic_fetch_add(&timer_state.cancel_count, 1ULL,
                                       __ATOMIC_RELAXED);
            }
        }
        timer_unlock();
    }
    seL4_SetMR(0, LUNA_COMMAND_TIMER_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    return error;
}

int luna_timer_manager_schedule_network(seL4_CPtr poll_ntfn,
                                        unsigned long long timeout_ns)
{
    if (!__atomic_load_n(&timer_state.ready, __ATOMIC_ACQUIRE) ||
        !poll_ntfn || !timeout_ns)
        return -1;
    int error = 0;
    timer_lock();
    if (!__atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE)) {
        unsigned long long cycles = ns_to_cycles(timeout_ns);
        if (!cycles) cycles = 1;
        __atomic_store_n(&timer_state.network_ntfn, poll_ntfn,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&timer_state.network_deadline_cycles,
                         __builtin_ia32_rdtsc() + cycles,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&timer_state.network_armed, 1, __ATOMIC_RELEASE);
        error = program_next(__builtin_ia32_rdtsc());
        if (error)
            __atomic_store_n(&timer_state.network_armed, 0,
                             __ATOMIC_RELEASE);
        else
            __atomic_fetch_add(&timer_state.network_arm_count, 1ULL,
                               __ATOMIC_RELAXED);
    }
    timer_unlock();
    return error;
}

int luna_timer_manager_cancel_network(void)
{
    if (!__atomic_load_n(&timer_state.ready, __ATOMIC_ACQUIRE)) return -1;
    if (!__atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE))
        return 0;
    timer_lock();
    int error = 0;
    if (__atomic_load_n(&timer_state.network_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&timer_state.network_armed, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&timer_state.network_ntfn, seL4_CapNull,
                         __ATOMIC_RELEASE);
        error = program_next(__builtin_ia32_rdtsc());
        if (!error)
            __atomic_fetch_add(&timer_state.network_cancel_count, 1ULL,
                               __ATOMIC_RELAXED);
    }
    timer_unlock();
    return error;
}

void luna_timer_manager_report(void)
{
    printf("LUNA_TIMER_NOTIFICATION_OK mechanism=pit-one-shot "
           "arms=%llu cancels=%llu interrupts=%llu wakes=%llu "
           "hardware_rearms=%llu polling_loops=0 network_arms=%llu "
           "network_cancels=%llu network_wakes=%llu\n",
           timer_state.arm_count, timer_state.cancel_count,
           timer_state.interrupt_count, timer_state.wake_count,
           timer_state.rearm_count, timer_state.network_arm_count,
           timer_state.network_cancel_count, timer_state.network_wake_count);
}
