/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sel4_lkl_host.h — seL4 后端为 LKL host_operations 提供的内部原语（Phase 1.1，已对齐真实 lkl/linux API）。
 *
 * 关键事实（见 DESIGN.md / 源码核对）：
 *   - lkl/linux fork 的 LKL 用 lkl_init(ops) 注入 host_ops，再 lkl_start_kernel(cmdline)。
 *   - "init 进程"即调用 lkl_start_kernel 的宿主线程本身（lkl_run_init binfmt 在 setup.c），
 *     不加载独立 ELF、不需要 initramfs/virtio；hello 由宿主直接 lkl_sys_write 完成。
 *   - liblkl.a 中只有 lkl.o（内核）是干净的：未定义符号仅 lkl_printf / lkl_bug，由本仓提供。
 *   - sel4utils_configure_thread 已为派生 TCB 设置 TLS（sel4runtime_write_tls_image + SetTLSBase），
 *     故 __thread 变量在每个 LKL 宿主线程中独立可用 —— thread_self() 据此实现。
 */
#ifndef SEL4_LKL_HOST_H
#define SEL4_LKL_HOST_H

#include <sel4/sel4.h>
#include <vka/vka.h>
#include <vka/object.h>
#include <vspace/vspace.h>
#include <simple/simple.h>
#include <sel4utils/thread.h>
#include <platsupport/io.h>
#include <lkl_host.h>

#define LKL_MAX_THREADS 64
#define LKL_MAX_TLS_KEYS 16

struct sel4_lkl_ctx {
    simple_t        *simple;
    vka_t           *vka;
    vspace_t        *vspace;
    seL4_CPtr        cspace;
    seL4_CPtr        root_tcb;
    seL4_CPtr        fault_ep;
    vka_object_t     fault_ep_obj;
    ps_io_port_ops_t io_port_ops;   /* COM1 轮询输入用 */
    int              io_port_inited;

    /* PIT 校准后的 TSC 单调时钟 + polling oneshot timer。 */
    sel4utils_thread_t timer_thread;
    int              timer_inited;
    int              timer_thread_started;
    volatile int     timer_stop;
    volatile int     timer_armed;
    unsigned long long timer_deadline_ns;
    unsigned long long tsc_freq;
    unsigned long long tsc_epoch_ns;
    void           (*timer_fn)(void);   /* LKL 注册的回调 → lkl_trigger_irq(TIMER_IRQ) */
};

extern struct sel4_lkl_ctx g_ctx;
extern struct lkl_host_operations seL4_lkl_host_ops;

int sel4_lkl_host_init(simple_t *simple, vka_t *vka, vspace_t *vspace,
                       seL4_CPtr cspace, seL4_CPtr root_tcb);
int sel4_lkl_host_thread_reuse_test(void);
unsigned long long sel4_lkl_host_time(void);
int sel4_lkl_host_console_ready(void);
void sel4_lkl_host_stop_console(void);
void sel4_lkl_host_shutdown(void);

#endif /* SEL4_LKL_HOST_H */
