/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host operations migrated into luna-lkl-task.  This first slice is sufficient
 * for lkl_init()/lkl_cleanup(); thread, synchronization, time and device ops
 * are added before moving lkl_start_kernel().
 */
#include "luna_lkl_task_host.h"

#include <sel4/sel4.h>
#include <sys/types.h>
#include <lkl_host.h>
#include <string.h>

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
    .memcpy = task_memcpy,
    .memset = task_memset,
    .memmove = task_memmove,
};

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
