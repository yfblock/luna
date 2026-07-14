/* SPDX-License-Identifier: GPL-2.0 */
/*
 * main.c — seL4 root task：引导 LKL 并以“宿主即 init”模型打印 hello（Phase 1.1）。
 *
 * 流程（见 DESIGN.md）：
 *   1. 从 bootinfo 引导 simple/vka/vspace（root task 自用 + 供 LKL host_ops 分配 TCB/栈）。
 *   2. sel4_lkl_host_init：建 fault ntfn、ltimer + 定时器服务线程。
 *   3. lkl_init(&seL4_lkl_host_ops) 注入 host ops。
 *   4. lkl_start_kernel("mem=64M ...") —— LKL 内核在派生 TCB 上跑起来，
 *      完成 start_kernel → run_init_process("/init")（lkl_run_init binfmt 空操作）→
 *      sem_up(init_sem) + thread_exit，随后本函数（宿主线程）成为 init/PID1 返回。
 *   5. lkl_sys_write(1, "hello from LKL on seL4\n") —— 经 lkl_syscall → LKL sys_write →
 *      console → lkl_ops->print → seL4_DebugPutChar。
 *   6. lkl_sys_halt。
 */
#include "sel4_lkl_host.h"
#include <sel4/sel4.h>
#include <sel4runtime.h>
#include <simple-default/simple-default.h>
#include <allocman/allocman.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <vka/object.h>
#include <vspace/vspace.h>
#include <sel4utils/vspace.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/bootinfo.h>
#include <sel4platsupport/io.h>
#include <sel4platsupport/arch/io.h>
#include <platsupport/io.h>
#include <utils/util.h>
#include <lkl.h>
#include <stdio.h>

void luna_shell_run(void);
#include <stdio.h>

#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 256)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];

static simple_t             simple;
static allocman_t          *allocman;
static vka_t                vka;
static vspace_t             vspace;
static sel4utils_alloc_data_t vspace_data;

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
    seL4_CPtr cnode = simple_get_cnode(&simple);
    seL4_CPtr root_tcb = seL4_CapInitThreadTCB;

    int err = sel4utils_bootstrap_vspace_with_bootinfo(&vspace, &vspace_data, pd, &vka, info, NULL, NULL);
    ZF_LOGF_IF(err, "vspace bootstrap failed: %d", err);
    printf("luna: vspace ok\n");

    /* 2. 初始化 host 后端（fault ntfn + ltimer + 定时器服务线程） */
    err = sel4_lkl_host_init(&simple, &vka, &vspace, cnode, root_tcb);
    ZF_LOGF_IF(err, "lkl host init failed");
    printf("luna: host backend ok\n");

    /* 3. 注入 host ops */
    err = lkl_init(&seL4_lkl_host_ops);
    ZF_LOGF_IF(err, "lkl_init failed: %d", err);
    printf("luna: lkl_init ok\n");

    /* 4. 引导 LKL 内核 */
    printf("luna: calling lkl_start_kernel...\n");
    err = lkl_start_kernel("mem=16M loglevel=8");
    printf("luna: lkl_start_kernel returned %d\n", err);
    ZF_LOGF_IF(err, "lkl_start_kernel failed: %d", err);

    /* 5. 宿主即 init：打印 hello。
       LKL 控制台为 printk-console（无 tty 设备节点，/dev/console 返回 -ENODEV），
       故用 LKL 控制台 API lkl_printf（→ lkl_ops->print → seL4_DebugPutChar）输出。 */
    lkl_printf("hello from LKL on seL4\n");
    printf("luna: hello emitted via LKL console\n");

    /* 5b. 虚拟串口 + 交互 shell：mknod /dev/ttyLKL0(240:0)，打开为 fd 0/1/2。
       LKL 的 lkl_tty 驱动把该 tty 的输入接 seL4 COM1（IRQ 注入）、输出接 seL4_DebugPutChar。
       shell 经 lkl_sys_read(0)/write(1) 走 tty，ldisc 自动回显+行编辑。 */
    lkl_sys_mkdir("/dev", 0755);
    lkl_sys_mknod("/dev/ttyLKL0", LKL_S_IFCHR | 0666, (240 << 8) | 0);
    lkl_sys_close(0); lkl_sys_close(1); lkl_sys_close(2);   /* 确保空出 */
    long tfd = lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);  /* -> fd 0 */
    printf("luna: /dev/ttyLKL0 first open fd=%ld\n", tfd);
    if (tfd >= 0) {
        lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);  /* -> fd 1 */
        lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);  /* -> fd 2 */
        lkl_sys_mkdir("/proc", 0555);
        lkl_sys_mkdir("/tmp", 0755);
        lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
        luna_shell_run();
    }

    /* 6. 关停（halt 路径在 LKL 协作模型下会触发 reboot，Phase 1 接受其后的次生 fault） */
    lkl_sys_halt();
    lkl_cleanup();
    return 0;
}
