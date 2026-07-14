/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lkl_host_ops.c — struct lkl_host_operations 的 seL4 后端（Phase 1.1）。
 *
 * 对齐 lkl/linux fork 真实 API（arch/lkl/include/uapi/asm/host_ops.h）：
 *   lkl_init(ops) 注入本表 → lkl_start_kernel(cmdline)。
 *
 * 实现：
 *   线程  = sel4utils TCB（共享 root VSpace/CSpace；configure_thread 自动建 TLS）；
 *   互斥量 = Notification 二值信号量 + owner 计数（支持 recursive）；
 *   信号量 = Notification 二值信号量（Signal/Recv，latch 不丢唤醒）；
 *   TLS   = [tid][key] 表，tid 由 __thread current_tid 给出（派生 TCB 已有独立 TLS）；
 *   定时器 = PIT 校准 TSC + seL4 polling 服务线程，避开 pc99 IRQ 重复占用；
 *   内存  = 静态 BSS bump heap；mmap/shmem 退化为 bump 分配（单地址空间）；
 *   上下文切换 = musl setjmp/longjmp（lkl_jmp_buf.buf 当 jmp_buf 用）；
 *   控制台 = seL4_DebugPutChar（debug kernel）。
 *
 * 另提供 lkl.o 仅有的两个未定义符号：lkl_printf / lkl_bug。
 */
#include "sel4_lkl_host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <sel4/sel4.h>
#include <vka/object.h>
#include <vka/capops.h>
#include <sel4utils/helpers.h>
#include <sel4utils/thread.h>
#include <sel4utils/thread_config.h>
#include <sel4platsupport/platsupport.h>
#include <sel4platsupport/arch/io.h>
#include <utils/util.h>
#include <platsupport/arch/tsc.h>
#include <platsupport/plat/pit.h>
#include <lkl/asm/irq.h>          /* lkl_trigger_irq */

struct sel4_lkl_ctx g_ctx;
struct lkl_host_operations seL4_lkl_host_ops;
/* lkl_ops 在 lkl.o 中是 hidden 可见性，外部不可见；lkl_printf/lkl_bug 直接用本表的 .print/.panic */
static void *bump_alloc(unsigned long sz, unsigned long align);  /* 静态 BSS 堆分配器，见内存段 */
static void tls_cleanup_thread(lkl_thread_t tid);

/* ───────── 线程 ───────── */
struct lkl_thread {
    int                 used;
    lkl_thread_t        tid;
    void              (*fn)(void *);
    void               *arg;
    sel4utils_thread_t  t;
    vka_object_t        join_ntfn;
};

static struct lkl_thread g_threads[LKL_MAX_THREADS];
static __thread lkl_thread_t current_tid = 0;   /* 派生 TCB 各自独立 TLS */

static struct lkl_thread *thread_by_tid(lkl_thread_t tid)
{
    if (tid == 0 || tid > LKL_MAX_THREADS) return NULL;
    struct lkl_thread *lt = &g_threads[tid - 1];
    return lt->used && lt->tid == tid ? lt : NULL;
}

static void lkl_thread_trampoline(void *arg0, void *arg1, void *ipc_buf)
{
    (void)arg1; (void)ipc_buf;
    struct lkl_thread *lt = (struct lkl_thread *)arg0;
    current_tid = lt->tid;
    lt->fn(lt->arg);
    tls_cleanup_thread(current_tid);
    seL4_Signal(lt->join_ntfn.cptr);
    seL4_TCB_Suspend(lt->t.tcb.cptr);
    for (;;) seL4_Recv(g_ctx.fault_ep, NULL);
}

static lkl_thread_t host_thread_create(void (*f)(void *), void *arg)
{
    struct lkl_thread *lt = NULL;
    /* slot 0/tid 1 永久属于根线程；派生线程的 tid 等于 slot+1，可安全复用。 */
    for (int i = 1; i < LKL_MAX_THREADS; i++)
        if (!g_threads[i].used) { lt = &g_threads[i]; memset(lt, 0, sizeof(*lt)); lt->used = 1; break; }
    if (!lt) return 0;
    lt->tid = (lkl_thread_t)((lt - g_threads) + 1);
    lt->fn = f; lt->arg = arg;

    if (vka_alloc_notification(g_ctx.vka, &lt->join_ntfn)) { lt->used = 0; return 0; }

    sel4utils_thread_config_t cfg = {0};
    cfg.fault_endpoint = g_ctx.fault_ep;
    cfg.cspace = g_ctx.cspace; cfg.cspace_root_data = 0;
    cfg.custom_stack_size = 1; cfg.stack_size = 64;    /* 256 KiB 栈：足够 LKL start_kernel 且不致 vspace 映射出洞 */
    int err = sel4utils_configure_thread_config(g_ctx.vka, g_ctx.vspace, g_ctx.vspace, cfg, &lt->t);
    if (err) goto fail;

    err = sel4utils_start_thread(&lt->t, lkl_thread_trampoline, lt, NULL, 1);
    if (err) { sel4utils_clean_up_thread(g_ctx.vka, g_ctx.vspace, &lt->t); goto fail; }

    return lt->tid;
fail:
    vka_free_object(g_ctx.vka, &lt->join_ntfn);
    lt->used = 0;
    return 0;
}

static void host_thread_detach(void) { }

static void host_thread_exit(void)
{
    struct lkl_thread *lt = thread_by_tid(current_tid);
    if (lt) {
        tls_cleanup_thread(current_tid);
        seL4_Signal(lt->join_ntfn.cptr);
        seL4_TCB_Suspend(lt->t.tcb.cptr);
    }
    for (;;) seL4_Recv(g_ctx.fault_ep, NULL);
}

static int host_thread_join(lkl_thread_t tid)
{
    /* LKL 把调用 lkl_start_kernel() 的宿主根线程视为 init/PID1。
       lkl_sys_halt() 会 join 该 host task；它不是由 thread_create 创建的，
       因此没有 join Notification，join 在这里应直接成功。 */
    if (tid == 1) return 0;
    struct lkl_thread *lt = thread_by_tid(tid);
    if (!lt) return -1;
    seL4_Word badge;
    seL4_Recv(lt->join_ntfn.cptr, &badge);
    sel4utils_clean_up_thread(g_ctx.vka, g_ctx.vspace, &lt->t);
    vka_free_object(g_ctx.vka, &lt->join_ntfn);
    lt->used = 0;
    return 0;
}

static lkl_thread_t host_thread_self(void) { return current_tid; }
static int host_thread_equal(lkl_thread_t a, lkl_thread_t b) { return a == b; }
static void *host_thread_stack(unsigned long *size) { (void)size; return NULL; }

/* ───────── 同步：Notification 信号量 ───────── */
struct lkl_sem   { vka_object_t ntfn; };
struct lkl_mutex { vka_object_t ntfn; lkl_thread_t owner; int count; int recursive; };

static void ntsem_down(seL4_CPtr n) { seL4_Word b; seL4_Recv(n, &b); }
static void ntsem_up(seL4_CPtr n)   { seL4_Signal(n); }

static struct lkl_sem *host_sem_alloc(int count)
{
    struct lkl_sem *s = bump_alloc(sizeof(*s), 16);
    if (!s) return NULL;
    if (vka_alloc_notification(g_ctx.vka, &s->ntfn)) return NULL;
    for (int i = 0; i < count; i++) seL4_Signal(s->ntfn.cptr);  /* prime；count>1 塌缩为 1 */
    return s;
}
static void host_sem_free(struct lkl_sem *s)
{
    if (!s) return;
    vka_free_object(g_ctx.vka, &s->ntfn);
    /* bump：不回收结构体 */
}
static void host_sem_up(struct lkl_sem *s)   { ntsem_up(s->ntfn.cptr); }
static void host_sem_down(struct lkl_sem *s) { ntsem_down(s->ntfn.cptr); }

static struct lkl_mutex *host_mutex_alloc(int recursive)
{
    struct lkl_mutex *m = bump_alloc(sizeof(*m), 16);
    if (!m) return NULL;
    if (vka_alloc_notification(g_ctx.vka, &m->ntfn)) return NULL;
    seL4_Signal(m->ntfn.cptr);   /* prime：未上锁 */
    m->owner = 0; m->count = 0; m->recursive = recursive;
    return m;
}
static void host_mutex_free(struct lkl_mutex *m)
{
    if (!m) return;
    vka_free_object(g_ctx.vka, &m->ntfn);
}
static void host_mutex_lock(struct lkl_mutex *m)
{
    lkl_thread_t self = current_tid;
    if (m->recursive && m->owner == self) { m->count++; return; }
    ntsem_down(m->ntfn.cptr);
    m->owner = self; m->count = 1;
}
static void host_mutex_unlock(struct lkl_mutex *m)
{
    if (m->recursive && m->owner == current_tid) {
        if (--m->count > 0) return;
    }
    m->owner = 0; m->count = 0;
    ntsem_up(m->ntfn.cptr);
}

/* ───────── TLS：[tid][key] 表 ───────── */
struct lkl_tls_key { int used; void *data[LKL_MAX_THREADS + 1]; void (*destructor)(void *); };
static struct lkl_tls_key g_tls[LKL_MAX_TLS_KEYS];

static void tls_cleanup_thread(lkl_thread_t tid)
{
    if (!tid || tid > LKL_MAX_THREADS) return;
    for (int k = 0; k < LKL_MAX_TLS_KEYS; k++) {
        void *data = g_tls[k].data[tid];
        g_tls[k].data[tid] = NULL;
        if (data && g_tls[k].used && g_tls[k].destructor)
            g_tls[k].destructor(data);
    }
}

static struct lkl_tls_key *host_tls_alloc(void (*destructor)(void *))
{
    for (int k = 0; k < LKL_MAX_TLS_KEYS; k++)
        if (!g_tls[k].used) {
            g_tls[k].used = 1; g_tls[k].destructor = destructor;
            memset(g_tls[k].data, 0, sizeof(g_tls[k].data));
            return &g_tls[k];
        }
    return NULL;
}
static void host_tls_free(struct lkl_tls_key *key)
{
    if (!key) return;
    if (key->destructor) {
        for (int t = 0; t <= LKL_MAX_THREADS; t++) {
            void *data = key->data[t];
            key->data[t] = NULL;
            if (data) key->destructor(data);
        }
    }
    key->destructor = NULL;
    key->used = 0;
}
static int host_tls_set(struct lkl_tls_key *key, void *data)
{
    if (!key || current_tid > LKL_MAX_THREADS) return -1;
    key->data[current_tid] = data;
    return 0;
}
static void *host_tls_get(struct lkl_tls_key *key)
{
    if (!key || current_tid > LKL_MAX_THREADS) return NULL;
    return key->data[current_tid];
}

/* ───────── 内存：bump 堆（避开派生线程 musl malloc→mmap→errno 的 TLS 缺陷） ───────── */
static char   g_heap_buf[32 * 1024 * 1024];   /* 32 MiB 静态 BSS，由 loader 映射，绕过 vspace 限额 */
static char  *g_heap = g_heap_buf;
static size_t g_heap_off;
static size_t g_heap_size = sizeof(g_heap_buf);
static void *bump_alloc(unsigned long sz, unsigned long align)
{
    uintptr_t base = (uintptr_t)g_heap + g_heap_off;
    base = (base + align - 1) & ~(align - 1);
    size_t need = sz ? sz : 1;
    if (base + need > (uintptr_t)g_heap + g_heap_size) return NULL;
    g_heap_off = (size_t)(base + need - (uintptr_t)g_heap);
    return (void *)base;
}

static void *host_mem_alloc(unsigned long sz)            { return bump_alloc(sz, 16); }
static void  host_mem_free(void *p)                       { (void)p; /* bump：不回收 */ }
static void *host_page_alloc(unsigned long sz)            { return bump_alloc(sz ? sz : 4096, 4096); }
static void  host_page_free(void *p, unsigned long sz)    { (void)p; (void)sz; }
static void *host_memcpy(void *d, const void *s, unsigned long n)  { return memcpy(d, s, n); }
static void *host_memset(void *s, int c, unsigned long n)          { return memset(s, c, n); }
static void *host_memmove(void *d, const void *s, unsigned long n) { return memmove(d, s, n); }
static void *host_mmap(void *addr, unsigned long sz, enum lkl_prot prot)
{ (void)addr; (void)prot; return bump_alloc(sz, 4096); }
static int   host_munmap(void *addr, unsigned long sz)    { (void)addr; (void)sz; return 0; }
static void  host_shmem_init(unsigned long sz)            { (void)sz; }
static void *host_shmem_mmap(void *addr, unsigned long pg_off, unsigned long sz, enum lkl_prot prot)
{ (void)addr; (void)pg_off; (void)prot; return bump_alloc(sz, 4096); }

/* ───────── 时间 / 定时器：PIT 校准 TSC，polling TCB 实现 oneshot ───────── */
static unsigned long long host_time(void)
{
    if (!g_ctx.timer_inited) return 0;
    return tsc_get_time(g_ctx.tsc_freq) - g_ctx.tsc_epoch_ns;
}
unsigned long long sel4_lkl_host_time(void) { return host_time(); }

static void *host_timer_alloc(void (*fn)(void))
{
    if (!g_ctx.timer_inited || !fn) return NULL;
    g_ctx.timer_fn = fn;
    __atomic_store_n(&g_ctx.timer_armed, 0, __ATOMIC_RELEASE);
    return &g_ctx;
}
static int host_timer_set_oneshot(void *timer, unsigned long ns)
{
    if (timer != &g_ctx || !g_ctx.timer_inited) return -1;
    unsigned long long deadline = host_time() + (ns ? ns : 1);
    __atomic_store_n(&g_ctx.timer_deadline_ns, deadline, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ctx.timer_armed, 1, __ATOMIC_RELEASE);
    return 0;
}
static void host_timer_free(void *timer)
{
    if (timer == &g_ctx)
        __atomic_store_n(&g_ctx.timer_armed, 0, __ATOMIC_RELEASE);
}

#define LKL_TIMER_TID 0x7ffffffdUL
static void timer_service_fn(void *arg0, void *arg1, void *ipc_buf)
{
    (void)arg0; (void)arg1; (void)ipc_buf;
    current_tid = LKL_TIMER_TID;
    while (!__atomic_load_n(&g_ctx.timer_stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&g_ctx.timer_armed, __ATOMIC_ACQUIRE)) {
            unsigned long long deadline = __atomic_load_n(&g_ctx.timer_deadline_ns, __ATOMIC_RELAXED);
            if (host_time() >= deadline &&
                __atomic_exchange_n(&g_ctx.timer_armed, 0, __ATOMIC_ACQ_REL)) {
                void (*fn)(void) = g_ctx.timer_fn;
                if (fn) fn();   /* → lkl_trigger_irq(LKL_TIMER_IRQ) */
            }
        }
        seL4_Yield();
    }
    seL4_TCB_Suspend(g_ctx.timer_thread.tcb.cptr);
}

/* ───────── 虚拟串口：host 侧 COM1 轮询 + SPSC 环 + IRQ 注入 ───────── */
#define LKL_TTY_RING 256
static unsigned char g_in_ring[LKL_TTY_RING];
static volatile unsigned g_in_head, g_in_tail;   /* SPSC: head=producer, tail=consumer */
static int g_tty_irq;
static sel4utils_thread_t g_poll_thread;
static int g_poll_thread_started;
static volatile int g_poll_stop;

/* COM1 轮询线程（独立 seL4 TCB，非 LKL 线程）：收到字符填环 + lkl_trigger_irq。
 * lkl_trigger_irq 会在此线程上跑 IRQ handler，lkl_cpu 用 thread_self() 当 owner id，
 * 故必须给一个非 0 的 current_tid（否则 cpu.owner=0 → put 时 "unbalanced put" panic）。 */
#define LKL_POLL_TID 0x7ffffffeUL
static void com1_poll_fn(void *a0, void *a1, void *ipc_buf)
{
    (void)a0; (void)a1; (void)ipc_buf;
    current_tid = LKL_POLL_TID;
    while (!__atomic_load_n(&g_poll_stop, __ATOMIC_ACQUIRE)) {
        uint32_t lsr = 0, c = 0;
        if (ps_io_port_in(&g_ctx.io_port_ops, 0x3fd, 1, &lsr) == 0 && (lsr & 0x01)) {
            if (ps_io_port_in(&g_ctx.io_port_ops, 0x3f8, 1, &c) == 0) {
                unsigned next = (g_in_head + 1) % LKL_TTY_RING;
                if (next != g_in_tail) {            /* 环未满 */
                    int was_empty = (g_in_head == g_in_tail);
                    g_in_ring[g_in_head] = (unsigned char)c;
                    g_in_head = next;
                    if (was_empty && g_tty_irq)
                        lkl_trigger_irq(g_tty_irq);  /* 注入 IRQ，LKL 在 IRQ 上下文取字符 */
                }
            }
        }
        seL4_Yield();   /* 单核 seL4：让步给 LKL 线程，避免独占 */
    }
    seL4_TCB_Suspend(g_poll_thread.tcb.cptr);
}

static void host_console_start(int irq)
{
    if (!g_ctx.io_port_inited || g_poll_thread_started) {
        if (!g_ctx.io_port_inited) ZF_LOGE("console input unavailable: no I/O port ops");
        return;
    }
    g_tty_irq = irq;
    __atomic_store_n(&g_poll_stop, 0, __ATOMIC_RELEASE);
    sel4utils_thread_config_t cfg = {0};
    cfg.fault_endpoint = g_ctx.fault_ep;
    cfg.cspace = g_ctx.cspace; cfg.cspace_root_data = 0;
    cfg.custom_stack_size = 1; cfg.stack_size = 8;
    if (sel4utils_configure_thread_config(g_ctx.vka, g_ctx.vspace, g_ctx.vspace, cfg, &g_poll_thread)) {
        ZF_LOGE("console poll thread create failed");
        return;
    }
    int err = sel4utils_start_thread(&g_poll_thread, com1_poll_fn, NULL, NULL, 1);
    if (err) {
        ZF_LOGE("console poll thread start failed: %d", err);
        sel4utils_clean_up_thread(g_ctx.vka, g_ctx.vspace, &g_poll_thread);
        memset(&g_poll_thread, 0, sizeof(g_poll_thread));
        return;
    }
    g_poll_thread_started = 1;
}

void sel4_lkl_host_stop_console(void)
{
    if (!g_poll_thread_started) return;
    __atomic_store_n(&g_poll_stop, 1, __ATOMIC_RELEASE);
    seL4_TCB_Suspend(g_poll_thread.tcb.cptr);
    sel4utils_clean_up_thread(g_ctx.vka, g_ctx.vspace, &g_poll_thread);
    memset(&g_poll_thread, 0, sizeof(g_poll_thread));
    g_poll_thread_started = 0;
    g_tty_irq = 0;
}

int sel4_lkl_host_console_ready(void) { return g_poll_thread_started; }

static int host_console_take(void)
{
    if (g_in_head == g_in_tail) return -1;
    unsigned char c = g_in_ring[g_in_tail];
    g_in_tail = (g_in_tail + 1) % LKL_TTY_RING;
    return (int)c;
}

/* ───────── iomem（Phase 1 无 virtio，不调用） ───────── */
static void *host_ioremap(long addr, int size) { (void)addr; (void)size; return NULL; }
static int host_iomem_access(const volatile void *addr, void *val, int size, int write)
{ (void)addr; (void)val; (void)size; (void)write; return -1; }

/* ───────── jmp_buf：musl setjmp/longjmp ───────── */
static void host_jmp_buf_set(struct lkl_jmp_buf *j, void (*f)(void))
{
    if (!setjmp(*((jmp_buf *)j->buf))) f();
}
static void host_jmp_buf_longjmp(struct lkl_jmp_buf *j, int val)
{
    longjmp(*((jmp_buf *)j->buf), val);
}

/* ───────── console / panic ───────── */
static void host_print(const char *str, int len)
{
    for (int i = 0; i < len; i++) {
#ifdef CONFIG_DEBUG_BUILD
        seL4_DebugPutChar(str[i]);
#else
        (void)str; break;
#endif
    }
}
static void host_panic(void)
{
    const char *m = "LKL host panic\n";
    host_print(m, 14);
#ifdef CONFIG_DEBUG_BUILD
    seL4_DebugHalt();
#endif
    for (;;) seL4_TCB_Suspend(g_ctx.root_tcb);
}

/* ───────── lkl.o 仅有的两个未定义符号 ───────── */
int lkl_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int n = r < 0 ? 0 : (r < (int)sizeof(buf) ? r : (int)sizeof(buf) - 1);
    if (seL4_lkl_host_ops.print && n) seL4_lkl_host_ops.print(buf, n);
    return r;
}
void lkl_bug(const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int n = r < 0 ? 0 : (r < (int)sizeof(buf) ? r : (int)sizeof(buf) - 1);
    if (seL4_lkl_host_ops.print && n) seL4_lkl_host_ops.print(buf, n);
    if (seL4_lkl_host_ops.panic) seL4_lkl_host_ops.panic();
    for (;;);
}

/* ───────── 后端初始化 ───────── */
int sel4_lkl_host_init(simple_t *simple, vka_t *vka, vspace_t *vspace,
                       seL4_CPtr cspace, seL4_CPtr root_tcb)
{
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.simple = simple; g_ctx.vka = vka; g_ctx.vspace = vspace;
    g_ctx.cspace = cspace; g_ctx.root_tcb = root_tcb;

    if (vka_alloc_endpoint(vka, &g_ctx.fault_ep_obj)) return -1;
    g_ctx.fault_ep = g_ctx.fault_ep_obj.cptr;

    /* COM1 I/O 端口（虚拟串口输入轮询用）。DebugPutChar 模式下 new_io_ops 提前返回，故直接取。 */
    if (sel4platsupport_get_io_port_ops(&g_ctx.io_port_ops, simple, vka))
        return -1;
    g_ctx.io_port_inited = 1;

    /* bump 堆为静态 BSS（g_heap_buf），无需在此分配 */

    /* 根线程（即调用 lkl_start_kernel 的宿主，LKL 视作 init/PID1）需要一个非 0 的 tid：
       LKL 约定 tid 0 = “无线程”，lkl_cpu_put 在 owner==0 时会 panic。 */
    current_tid = 1;
    g_threads[0].used = 1; g_threads[0].tid = 1;

    /* 默认 ltimer 会通过两条路径获取同一 pc99 IRQ。这里只用 PIT 做一次 TSC
       频率校准，不申请 IRQ；之后 host_time 和 oneshot 都基于单调 TSC。 */
    pit_t pit;
    if (pit_init(&pit, g_ctx.io_port_ops)) return -1;
    g_ctx.tsc_freq = tsc_calculate_frequency_pit(&pit);
    pit_cancel_timeout(&pit);
    if (!g_ctx.tsc_freq) return -1;
    g_ctx.tsc_epoch_ns = tsc_get_time(g_ctx.tsc_freq);
    g_ctx.timer_inited = 1;

    sel4utils_thread_config_t cfg = {0};
    cfg.fault_endpoint = g_ctx.fault_ep;
    cfg.cspace = cspace; cfg.cspace_root_data = 0;
    cfg.custom_stack_size = 1; cfg.stack_size = 8;
    int err = sel4utils_configure_thread_config(vka, vspace, vspace, cfg, &g_ctx.timer_thread);
    if (err) return -1;
    err = sel4utils_start_thread(&g_ctx.timer_thread, timer_service_fn, NULL, NULL, 1);
    if (err) {
        sel4utils_clean_up_thread(vka, vspace, &g_ctx.timer_thread);
        return -1;
    }
    g_ctx.timer_thread_started = 1;
    return 0;
}

static struct lkl_tls_key *g_thread_test_key;
static int g_thread_test_destructors;

static void thread_reuse_test_destructor(void *data)
{
    if (data == (void *)1) g_thread_test_destructors++;
}

static void thread_reuse_test_fn(void *arg)
{
    (void)arg;
    if (host_tls_set(g_thread_test_key, (void *)1))
        host_panic();
}

int sel4_lkl_host_thread_reuse_test(void)
{
    g_thread_test_destructors = 0;
    g_thread_test_key = host_tls_alloc(thread_reuse_test_destructor);
    if (!g_thread_test_key) return -1;
    for (int i = 0; i < LKL_MAX_THREADS + 16; i++) {
        lkl_thread_t tid = host_thread_create(thread_reuse_test_fn, NULL);
        if (!tid || host_thread_join(tid)) {
            host_tls_free(g_thread_test_key);
            g_thread_test_key = NULL;
            return -1;
        }
    }
    host_tls_free(g_thread_test_key);
    g_thread_test_key = NULL;
    return g_thread_test_destructors == LKL_MAX_THREADS + 16 ? 0 : -1;
}

void sel4_lkl_host_shutdown(void)
{
    sel4_lkl_host_stop_console();
    if (g_ctx.timer_thread_started) {
        __atomic_store_n(&g_ctx.timer_stop, 1, __ATOMIC_RELEASE);
        seL4_TCB_Suspend(g_ctx.timer_thread.tcb.cptr);
        sel4utils_clean_up_thread(g_ctx.vka, g_ctx.vspace, &g_ctx.timer_thread);
        g_ctx.timer_thread_started = 0;
    }
    g_ctx.timer_inited = 0;
    if (g_ctx.fault_ep_obj.cptr) {
        vka_free_object(g_ctx.vka, &g_ctx.fault_ep_obj);
        memset(&g_ctx.fault_ep_obj, 0, sizeof(g_ctx.fault_ep_obj));
        g_ctx.fault_ep = seL4_CapNull;
    }
}

/* ───────── 填表 ───────── */
struct lkl_host_operations seL4_lkl_host_ops = {
    .virtio_devices    = NULL,
    .print             = host_print,
    .panic             = host_panic,
    .console_start     = host_console_start,
    .console_take      = host_console_take,
    .sem_alloc         = host_sem_alloc,
    .sem_free          = host_sem_free,
    .sem_up            = host_sem_up,
    .sem_down          = host_sem_down,
    .mutex_alloc       = host_mutex_alloc,
    .mutex_free        = host_mutex_free,
    .mutex_lock        = host_mutex_lock,
    .mutex_unlock      = host_mutex_unlock,
    .thread_create     = host_thread_create,
    .thread_detach     = host_thread_detach,
    .thread_exit       = host_thread_exit,
    .thread_join       = host_thread_join,
    .thread_self       = host_thread_self,
    .thread_equal      = host_thread_equal,
    .thread_stack      = host_thread_stack,
    .tls_alloc         = host_tls_alloc,
    .tls_free          = host_tls_free,
    .tls_set           = host_tls_set,
    .tls_get           = host_tls_get,
    .mem_alloc         = host_mem_alloc,
    .mem_free          = host_mem_free,
    .page_alloc        = host_page_alloc,
    .page_free         = host_page_free,
    .time              = host_time,
    .timer_alloc       = host_timer_alloc,
    .timer_set_oneshot = host_timer_set_oneshot,
    .timer_free        = host_timer_free,
    .ioremap           = host_ioremap,
    .iomem_access      = host_iomem_access,
    .jmp_buf_set       = host_jmp_buf_set,
    .jmp_buf_longjmp   = host_jmp_buf_longjmp,
    .memcpy            = host_memcpy,
    .memset            = host_memset,
    .memmove           = host_memmove,
    .mmap              = host_mmap,
    .munmap            = host_munmap,
    .shmem_init        = host_shmem_init,
    .shmem_mmap        = host_shmem_mmap,
    .pci_ops           = NULL,
};
