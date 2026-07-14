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

int luna_lkl_task_configure_resources(
    const struct luna_task_thread_resource resources[LUNA_RESOURCE_SLOTS]);
int luna_lkl_task_thread_test(void);
int luna_lkl_task_init(void);
void luna_lkl_task_cleanup(void);

#endif /* LUNA_LKL_TASK_HOST_H */
