/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_LKL_TASK_HOST_H
#define LUNA_LKL_TASK_HOST_H

#include "luna_isolation_protocol.h"

#include <sel4/sel4.h>

struct luna_task_thread_resource {
    seL4_CPtr tcb;
    seL4_Word stack_top;
    seL4_Word stack_pages;
    seL4_Word ipc_buffer_addr;
    seL4_CPtr join_ntfn;
};

struct luna_task_sync_resource {
    seL4_CPtr ntfn;
};

int luna_lkl_task_configure_resources(
    const struct luna_task_thread_resource threads[LUNA_RESOURCE_SLOTS],
    const struct luna_task_sync_resource sync[LUNA_SYNC_SLOTS],
    seL4_CPtr console_io_port, seL4_CPtr control_ep,
    seL4_CPtr command_ep);
int luna_lkl_task_configure_disk(void *io_base, unsigned long io_size,
                                 unsigned long disk_size);
int luna_lkl_task_configure_net(void *io_base, unsigned long io_size,
                                seL4_Word mac_word0, seL4_Word mac_word1,
                                seL4_CPtr rx_ntfn);
void luna_lkl_task_manager_lock(void);
void luna_lkl_task_manager_unlock(void);
int luna_lkl_task_manager_request(enum luna_isolation_event event,
                                  seL4_Word value1, seL4_Word value2);
int luna_lkl_task_manager_request_value(enum luna_isolation_event event,
                                        seL4_Word value1,
                                        seL4_Word value2,
                                        seL4_Word *response_value);
int luna_lkl_task_thread_test(void);
int luna_lkl_task_sync_tls_test(void);
int luna_lkl_task_sync_tls_runtime_ok(void);
void luna_lkl_task_resource_stats(void);
int luna_lkl_task_prepare_time(unsigned long long tsc_frequency);
int luna_lkl_task_thread_timer_test(void);
int luna_lkl_task_allocator_test(void);
int luna_lkl_task_allocator_idle(void);
int luna_lkl_task_init(void);
int luna_lkl_task_disk_add(void);
int luna_lkl_task_net_add(void);
int luna_lkl_task_net_prepare(void);
int luna_lkl_task_net_smoke(void);
int luna_lkl_task_net_pressure_smoke(void);
int luna_lkl_task_net_tx_pressure_smoke(void);
int luna_lkl_task_net_finish(void);
int luna_lkl_task_disk_prepare(enum luna_isolation_mode mode);
int luna_lkl_task_disk_finish(void);
int luna_lkl_task_disk_cleanup_after_halt(void);
int luna_lkl_task_user_smoke(void);
int luna_lkl_task_user_shell(void);
int luna_lkl_task_start_kernel(unsigned long long tsc_frequency);
long luna_lkl_task_halt(void);
unsigned long long luna_lkl_task_time(void);
int luna_lkl_task_console_ready(void);
void luna_lkl_task_console_stop(void);

#endif /* LUNA_LKL_TASK_HOST_H */
