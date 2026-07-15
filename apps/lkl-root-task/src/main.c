/* SPDX-License-Identifier: GPL-2.0 */
/* seL4 root manager: bootstrap allocation, calibrate TSC, and own the
 * isolated LKL task's capabilities and lifecycle. LKL itself is not linked
 * into this executable. */
#include "luna_isolation.h"
#include <sel4/sel4.h>
#include <sel4runtime.h>
#include <simple-default/simple-default.h>
#include <allocman/allocman.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <vka/object.h>
#include <vspace/vspace.h>
#include <sel4utils/vspace.h>
#include <sel4utils/helpers.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/bootinfo.h>
#include <sel4platsupport/io.h>
#include <sel4platsupport/arch/io.h>
#include <platsupport/io.h>
#include <platsupport/arch/tsc.h>
#include <platsupport/plat/pit.h>
#include <utils/util.h>
#include <stdlib.h>

/* Loading the isolated LKL ELF and repeatedly mapping its manager-backed heap
 * creates metadata for several thousand frames and mappings. Keep this pool
 * independent from the physical Untyped budget. */
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 1024)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

static simple_t             simple;
static allocman_t          *allocman;
static vka_t                vka;
static vspace_t             vspace;
static sel4utils_alloc_data_t vspace_data;

static unsigned long long calibrate_tsc(simple_t *simple, vka_t *vka)
{
    ps_io_port_ops_t io_port_ops = {0};
    if (sel4platsupport_get_io_port_ops(&io_port_ops, simple, vka))
        return 0;
    pit_t pit;
    if (pit_init(&pit, io_port_ops)) {
        free(io_port_ops.cookie);
        return 0;
    }
    unsigned long long frequency = tsc_calculate_frequency_pit(&pit);
    pit_cancel_timeout(&pit);
    free(io_port_ops.cookie);
    return frequency;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);   /* musl 对非 tty 全缓冲；关掉以便即时看输出 */
    NAME_THREAD(seL4_CapInitThreadTCB, "luna-lkl-root");

    /* 1. 引导 simple / vka / vspace */
    printf("luna: start\n");
    seL4_BootInfo *info = platsupport_get_bootinfo();
    ZF_LOGF_IF(info == NULL, "no bootinfo");
    simple_default_init_bootinfo(&simple, info);

    allocman = bootstrap_use_current_simple(&simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    ZF_LOGF_IF(allocman == NULL, "allocman init failed");
    allocman_make_vka(&vka, allocman);

    seL4_CPtr pd = simple_get_pd(&simple);
    seL4_CPtr root_tcb = seL4_CapInitThreadTCB;

    int err = sel4utils_bootstrap_vspace_with_bootinfo(&vspace, &vspace_data, pd, &vka, info, NULL, NULL);
    ZF_LOGF_IF(err, "vspace bootstrap failed: %d", err);
    printf("luna: vspace ok\n");

    unsigned long long tsc_frequency = calibrate_tsc(&simple, &vka);
    ZF_LOGF_IF(!tsc_frequency, "TSC calibration failed");

    /* Boot/halt LKL only in the isolated task, diagnose its deliberate fault,
       then rebuild a clean interactive child. */
    err = luna_isolation_smoke(&simple, &vka, &vspace, tsc_frequency);
    ZF_LOGF_IF(err, "isolation smoke failed");
    printf("LUNA_ISOLATION_OK\n");
    printf("LUNA_SHUTDOWN_OK\n");

    /* root task 没有父进程可返回；进入明确的 quiescent 状态，由测试端结束 QEMU。 */
    seL4_TCB_Suspend(root_tcb);
    for (;;) seL4_Yield();
}
