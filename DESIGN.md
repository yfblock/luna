# Phase 1.1 — LKL on seL4：基础移植设计

目标：把 **LKL (Linux Kernel Library)** 作为 seL4 的一个**纯用户态 Task**跑起来，引导 LKL 内核，
并在其内启动一个**静态链接的 HelloWorld**。**不涉及 UINTR**，仅验证 LKL 能在 seL4 用户态正常初始化与运行。

---

## 0. 关键事实（决定整个架构）

在动手前先厘清两个容易被误解的点，它们直接决定了移植方案：

1. **LKL 的“用户态进程”并不依赖硬件特权陷阱。**
   LKL 把 Linux 内核编译成库；内核线程与用户进程都跑在**宿主线程**上，内核态/用户态的切换由
   `setjmp/longjmp` 在**同一个宿主线程**内完成（`arch/lkl/kernel/lkl_cpu.c`、`process.c`）。
   用户进程发起的“syscall”不是 `syscall` 指令陷入，而是**函数调用 `lkl_syscall()`**。
   ⇒ 因此，静态 hello 必须链接 **LKL 的 syscall shim**（lkl-musl 或 LKL 应用 libc），使其 libc 的
   syscall stub 改走 `lkl_syscall`。普通静态 glibc/musl 二进制发出的裸 `syscall` 指令**不会**进入 LKL。
   （动态二进制可用 `LD_PRELOAD` 的 hijack 方式，但静态二进制只能靠重新链接 shim —— 本项目采用后者。）

2. **LKL 是单地址空间（single-address-space）的 library OS。**
   内核与所有 Linux 用户进程共享同一份宿主地址空间，进程间没有硬件 MMU 隔离。
   ⇒ 这正好**契合 seL4**：我们把**整个 LKL（内核 + 所有 Linux 进程）放进同一个 seL4 VSpace**，
   用 **seL4 TCB 实现 LKL 的每一个 host 线程**，不需要为每个 Linux 进程单独建 VSpace/ASID，
   也不需要任何硬件 syscall 陷入机制。这是 LKL 相对 full-Linux 移植到 seL4 的最大简化。

这两点合起来回答了“最大阻碍”那一节：seL4 的能力模型要求显式 cap，而 LKL 又是单地址空间 +
软件上下文切换 —— 所以**唯一真正的难点是“运行时动态创建 TCB / Notification / 栈并放置 cap”**，
而不是页表或陷入（见 §5）。

---

## 1. LKL Host Operations 适配层

LKL 通过 `struct lkl_host_operations`（见 `arch/lkl/include/asm/host_ops.h`，**以你的 LKL 版本为准**）
与宿主交互。我们在 seL4 用户态实现一个后端 `lkl_host_ops`，映射如下：

| LKL host op | POSIX 后端做的事 | seL4 后端实现 |
|---|---|---|
| `mem_alloc` / `mem_free` | `malloc` / `free` | 复用 muslc 的 `malloc`（由 `libsel4muslcsys` + `vka`/`vspace` 在 root task 提供，底层从 Untyped 分配页并映射） |
| `mem_cache_alloc/free` | `aligned_alloc` | 同上，带对齐 |
| `print` | `write(2)` | `seL4_DebugPutChar` 循环（debug kernel）；release 用 `libsel4platsupport` 串口驱动 |
| `panic` | `abort` | 打印 + `seL4_DebugHalt()` 死循环 |
| `thread_create/destroy/join/self` | `pthread_create/join` | `vka_alloc_tcb` + `sel4utils_create_thread`（共享 root VSpace/CSpace）+ `seL4_TCB_WriteRegisters/Resume`；join 用 Notification |
| `mutex_*` | `pthread_mutex` | seL4 **Notification 二值信号量**（libsel4sync 风格） |
| `sem_*` | `sem_post/wait` | 同上；count>1 退化为计数版（counter + Notification + 锁） |
| `tls_alloc/set/get/free` | `pthread_key_*` | 小型二维表 `[tid][key]`，tid 由 `__thread` 变量 + TCB TLS base 提供 |
| `timer_alloc/free/oneshot/periodic` | `timer_create` | seL4 定时器服务线程：`libsel4platsupport`/`ltimer` 驱动 + Notification 绑定 TCB，tick 回调 `lkl_trigger_irq(LKL_TIMER_IRQ)` |
| `time` / `timer_current` | `clock_gettime` | ltimer `get_time`，或由 tick 累计 |
| `get_cpu`/`set_cpu` | CPU affinity | Phase 1 单核：返回/忽略 cpu 0 |

### 1.1 内存

root task（也是 LKL 的宿主进程）由 sel4runtime 启动，muslc 的 `malloc` 已被 `libsel4muslcsys`
接管：`brk`/`mmap` 转成从 `vka` 分配 Untyped、`vspace` 映射页。所以 `lkl_host_ops.mem_alloc` 直接
`return malloc(size)` 即可，LKL 的内核堆与用户堆共享这一 arena。

> **要点**：Phase 1 让 LKL 与 root task **共用同一个 VSpace**，因此共用同一套堆。若日后要把 LKL 放进
> **独立子任务**（独立 VSpace），则需要：(a) 给子任务一个独立 CSpace；(b) 预分配一段堆区（`vspace_new_pages`）
> 并把首地址传给子任务自带的 malloc；(c) 子任务的 `mem_alloc` 走自己 VSpace 的页分配器。Phase 1 不做。

### 1.2 线程（`thread_create/join/destroy/self`）

每个 LKL host 线程 = 一个 seL4 TCB，运行在**共享 root VSpace / 共享 root CSpace** 内：

```c
/* sel4_threads.c —— 概要 */
struct lkl_thread {
    sel4utils_thread_t t;     /* 含 TCB cap、栈 */
    seL4_CPtr       join_ntfn;/* 退出时 signal，供 thread_join 阻塞 */
    int             tid;      /* 全局自增 id */
    _Bool           alive;
};

int sel4_thread_create(void (*fn)(void *), void *arg) {
    struct lkl_thread *lt = alloc_slot();
    sel4utils_thread_config_t cfg = thread_config_default(vspace, cspace);
    cfg.fault_endpoint = fault_handler_ntfn;   /* 统一 fault EP */
    cfg.tls_create     = true;                 /* 见 §1.4：必须给 TLS */
    sel4utils_create_thread(vka, &lt->t, &cfg, fn);
    /* 在新线程入口里先 tls_set(TID_KEY, lt->tid) 再跑 fn(arg) */
    lt->join_ntfn = vka_alloc_notification();
    lt->alive = true;
    sel4utils_start_thread(&lt->t, lkl_thread_trampoline, lt, /*resume=*/1);
    return lt->tid;
}

int sel4_thread_join(int tid) {
    struct lkl_thread *lt = find(tid);
    if (lt->alive) seL4_Wait(lt->join_ntfn, NULL);   /* 阻塞至退出 */
    return 0;
}

void sel4_thread_destroy(void) {   /* 当前线程退出 */
    int tid = sel4_thread_self();
    struct lkl_thread *lt = find(tid);
    seL4_Signal(lt->join_ntfn);    /* 唤醒 joiner */
    /* 清理：vka_free_tcb / 回收栈 / free_slot */
    seL4_TCB_Suspend(self_tcb);
    /* 不返回 */
}
```

`thread_self()` 读 `__thread int lkl_cur_tid`（见 §1.4）。

### 1.3 同步原语（mutex / semaphore）

seL4 的 Endpoint `Send` 在没有接收者时会**阻塞**，不能直接当计数信号量用（`sem_up` 不能阻塞）。
正确且最简单的原语是 **Notification 作为二值信号量**（libsel4sync `sync_bin_sem` 同款）：

```c
struct lkl_mutex { seL4_CPtr ntfn; };      /* 二值信号量 */
struct lkl_sem   { seL4_CPtr ntfn; };

/* 初始化：先 signal 一次“放出一个令牌” */
seL4_Signal(ntfn);

static void bin_down(seL4_CPtr n) {
    seL4_Word badge;
    if (seL4_Poll(n, &badge) == 0)         /* 非阻塞取令牌 */
        seL4_Wait(n, &badge);              /* 没拿到则阻塞 */
}
static void bin_up(seL4_CPtr n) { seL4_Signal(n); }
```

`mutex_lock`=bin_down，`mutex_unlock`=bin_up，`sem_alloc(0)` 不预置令牌、`sem_up`/`sem_down` 同理。

> **限制与对策**：Notification 的 word 是位掩码，多次 `Signal` 同一 bit 会**塌缩成 1**，因此本实现是
> **二值**信号量。LKL 的实际用法（`lkl_cpu` 的 sem 初始为 0、至多一个在途令牌；softirq sem 等）都是 0/1
> 语义，二值够用。若日后遇到 `sem_alloc(count>1)`，则改用“原子 counter + Notification + 一把 mutex”
> 的用户态计数信号量（见 `lkl_host_ops.c` 注释里的 `counting_sem_*` 变体）。

### 1.4 TLS（`tls_alloc/set/get/free`）

LKL 用 host TLS 存 `current` 等每线程数据。我们两件事并行：

- **给每个 seL4 TCB 配 TLS**：`sel4utils_create_thread(..., cfg.tls_create=true)` 会拷贝 master TLS 模板并
  `seL4_TCB_SetTLSBase`。这样 LKL arch 层用到的 `__thread` 变量（如 `__thread struct task_struct *current`）
  才能正确解析。这是移植里一个**常被忽略的坑**：不配 TLS base，LKL 一访问 `current` 就崩。
- **实现 `lkl_host_ops.tls_*`**：用一个 `[MAX_THREADS][MAX_TLS_KEYS]` 的小表；`tls_alloc` 返回一个 key，
  `tls_set/get` 用 `thread_self()` 当行索引读写。`thread_self()` 由 `__thread int lkl_cur_tid` 提供，
  该变量在新线程 trampoline 里**第一件事**就被赋值。

### 1.5 定时器（驱动 LKL 的时钟中断）

LKL 启动时调用 `host_ops.timer_periodic(lkl_timer_cb, NULL, NSEC_PER_SEC/LKL_HZ)`；我们让它驱动一个
**独立的 seL4 定时器服务线程**：

```
            ltimer/serial-irq (periodic)
                     │ signal
                     ▼
        ┌─────────────────────────┐
        │  timer Notification     │  ← bound to timer-TCB
        └────────────┬────────────┘
                     │ seL4_Recv(ntfn)
                     ▼
        ┌─────────────────────────┐
        │  timer service TCB      │  (共享 VSpace)
        │  while(1){ Recv;         │
        │    lkl_timer_cb(NULL);}  │  → lkl_trigger_irq(LKL_TIMER_IRQ)
        └─────────────────────────┘
```

- `timer_alloc`：`ltimer_init` 拿到定时器 + 一个 Notification cap，把它 **bind 到 timer 服务 TCB**
  （`seL4_TCB_BindNotification`）。
- `timer_periodic(cb, arg, ns)`：保存 cb/arg，`ltimer_set_timeout(ns)` 周期触发。
- 服务线程 `seL4_Recv(timer_ntfn, &badge)` 返回即调用 `cb(arg)` → `lkl_trigger_irq(LKL_TIMER_IRQ)`。
- `timer_oneshot`：Phase 1 用周期定时近似（或 ltimer 单次模式）。

> **关键**：定时器中断是**软件事件**，由 timer TCB 主动把 IRQ “投递”给 LKL 内核（LKL 是协作式处理 IRQ：
  `lkl_trigger_irq` 置位后由 CPU 线程在调度点检查），**不需要跨 TCB 硬抢占**，因此也不需要 MCS 的
  scheduling-context cap 来抢占 CPU 线程。这是把 LKL 放 seL4 的另一大便利。

### 1.6 控制台 / 时间 / panic

- `print`：`for (i) seL4_DebugPutChar(str[i])`。**仅在 debug kernel 可用**；release kernel 必须接
  `libsel4platsupport` 串口驱动（占用一个 IRQ cap + device untyped）。Phase 1 用 debug 路径快速验证。
- `time`：`ltimer_get_time` 单调纳秒；或由 tick 计数 × tick_ns 近似。
- `panic`：`print("LKL PANIC...")` 后 `seL4_DebugHalt()`（或 `while(1) seL4_TCB_Suspend(self)`）。

---

## 2. seL4 启动环境配置

### 2.1 启动流程

```
seL4 boot
  └─ root task (sel4runtime) main()
       ├─ 1. 从 sel4runtime env 取 simple/vka/vspace + io_ops
       ├─ 2. 初始化 host 后端：fault ntfn、TLS 表、timer 驱动、串口(可选)
       ├─ 3. 准备 initramfs（已链接进映像的 __lkl__initrd_start/_end）
       ├─ 4. lkl_start_kernel(&lkl_host_ops, MEM_SIZE,
       │                      "mem=%luM loglevel=8 init=/init");
       │      └─ LKL 内部 thread_create(boot_cpu_fn) → 我们的 sel4_thread_create
       │           → 新 seL4 TCB 跑 start_kernel()
       │                → 挂载 initramfs → run_init_process("/init")
       │                → execve 静态 hello（其 syscall 走 lkl_syscall）
       ├─ 5. lkl_syscalls_init(); 等 boot CPU（join）
       └─ 6. hello 打印 → lkl_host_ops.print → seL4_DebugPutChar → 串口
```

root task 自己**就是 LKL 的宿主进程**（Phase 1 同址运行）；boot CPU 是 root 通过 `thread_create` 派生的
一个 seL4 TCB。timer 服务线程同理。

### 2.2 需要的 Capabilities

root task 由 seL4 bootinfo 自动授予以下 cap（Phase 1 直接复用，无需手动 mint）：

| Cap | 用途 |
|---|---|
| **CSpace root** | 存放所有派生 cap（TCB/Notification/Endpoint/Page）。LKL 线程共享之 |
| **VSpace root** | LKL 内核 + 所有 Linux 进程的唯一地址空间 |
| **全部 Untyped** | `vka` 用以分配 TCB、栈页、Notification、Endpoint |
| **ASID pool** | 映射页（root VSpace 已绑定） |
| **root TCB** | 绑定 timer Notification、设置优先级 |
| **fault endpoint / Notification** | 派生线程的 fault 入口（统一 handler） |
| **IRQ controller + 设备 Untyped**（libsel4platsupport `simple` 提供） | 定时器 / 串口驱动所需 |

为 LKL **boot CPU / timer / kthread** 派生 TCB 时，每个 TCB 需要：
1. 一个 TCB cap（`vka_alloc_tcb`）；
2. 一段栈（`vspace_new_pages` 映射若干页，通常 64 KiB）；
3. 一个 fault endpoint（共享同一个 fault ntfn）；
4. 一个 join Notification（供 `thread_join`）；
5. （timer 线程）一个绑定到 TCB 的 timer Notification。

### 2.3 栈与堆的分配与授权

- **栈**：由 `sel4utils_create_thread` 在共享 VSpace 内分配并映射（默认 1 页可调）。线程入口 PC 与 SP
  通过 `seL4_TCB_WriteRegisters` 写入，`seL4_TCB_Resume` 启动。
- **堆**：Phase 1 与 root task 共用 muslc arena（如 §1.1）。若用独立子任务：root 先 `vspace_new_pages`
  划出 N MiB 堆区，把 `[base, end)` 通过寄存器/共享内存传给子任务，子任务用自己的 `malloc` 管理该区。

---

## 3. 启动首个应用（静态 HelloWorld）

### 3.1 构建 hello（关键：必须链接 LKL syscall shim）

`apps/hello/hello.c`：

```c
/* 用 lkl 的应用 libc 编译：其 write/exit 等 stub 改走 lkl_syscall */
#include <unistd.h>
int main(void) { write(1, "hello from LKL on seL4\n", 23); return 0; }
```

编译（伪命令，具体路径取决于 LKL 树）：
```
$CC -static -nostdlib hello.c \
    -I$LKL/tools/lkl/include \
    $LKL/tools/lkl/lib/liblkl.a \      # 提供 lkl_syscall 与 libc shim
    -o hello
```
`$LKL/tools/lkl/lib/` 内的应用 libc shim 把 `write/read/exit/...` 重定向到 `lkl_sys_*`。
（若 LKL 树提供 `lkl-musl`，则用 `-L$lkl_musl/lib -lc` 链接 musl 即可，效果一致。）

### 3.2 打包 initramfs

`tools/build-initramfs.sh` 把 hello 作为 `/init` 打成 cpio：
```sh
mkdir -p rootfs/bin
cp hello rootfs/init            # LKL 默认执行 /init
( cd rootfs && find . | cpio -o -H newc ) > initramfs.cpio
```

### 3.3 嵌入映像

把 cpio 作为数据符号链接进 root task，供 LKL 取用（LKL 通过 `__lkl__initrd_start/_end` 读取内置 initramfs）：

```asm
/* initramfs.S */
.section .rodata
.global __lkl__initrd_start
.global __lkl__initrd_end
__lkl__initrd_start:
.incbin "initramfs.cpio"
__lkl__initrd_end:
```

### 3.4 引导并 execve

```c
/* lkl_boot.c 概要 */
extern char __lkl__initrd_start[], __lkl__initrd_end[];

void boot_lkl(void) {
    /* LKL 会自动发现 __lkl__initrd_start/_end 作为内置 initramfs */
    long mem_mb = 64;
    int err = lkl_start_kernel(&lkl_host_ops, mem_mb << 20,
                  "mem=%ldM loglevel=8 init=/init rdinit=/init", mem_mb);
    if (err) panic("lkl_start_kernel failed: %d", err);

    /* 内核起来后，run_init_process("/init") 会 execve 静态 hello。
       若想显式控制，可在 host 侧（把 host 当作 init）这样跑： */
    long fd = lkl_sys_open("/init", LKL_O_RDONLY, 0);   /* 验证可见 */
    lkl_sys_close(fd);

    /* 也可以由 host 直接 execve：
       const char *argv[] = {"/init", NULL};
       lkl_sys_execve("/init", argv, NULL); */

    lkl_sys_halt();   /* 等内核停止 */
}
```

LKL 内核 `start_kernel` → `init/main.c` 标准路径 → `run_init_process("/init")` →
`do_execve` → ELF loader 把 hello 载入**同一 VSpace** 的新“用户”线程上下文 →
hello 的 `write` → `lkl_syscall(__NR_write, ...)` → LKL 的 `sys_write` →
console driver → `lkl_host_ops.print` → `seL4_DebugPutChar` → 串口输出。

---

## 4. 编译与构建要点（CMake）

顶层依赖三个源树（作为 git submodule 或 fetch）：
- `seL4`（内核）
- `seL4_tools`（含 `sel4runtime`、`libsel4muslcsys`、`libsel4utils`、`libsel4platsupport`、`cmake-tool`）
- `linux`（含 `tools/lkl`，构建 `liblkl.a`）

关键配置点：

1. **LKL 侧**：`make -C tools/lkl ARCH=lkl -j` 产出 `liblkl.a`（host 端 + arch/lkl 已内置）。
   注意 LKL 默认 host 后端是 POSIX；我们用自己的 `lkl_host_ops` 取代 `tools/lkl/lib/lkl-host.c`，
   因此链接时**不要**链 `lkl-host.o`，只链 `liblkl.a` 中内核部分 + `lkl_syscalls`。
2. **seL4 侧 root task**：用 `sel4runtime` 做 entry，链接 `libsel4`、`libsel4utils`、
   `libsel4platsupport`、`libsel4muslcsys`、`libsel4debug`、`muslc`。
3. **静态链接**：整个 root task **静态**链接（`-static` 或 sel4runtime 默认即静态 PIE/非 PIE），
   把 `liblkl.a` 一起链进去。LKL 的 `__lkl__initrd_start/_end` 符号在同一可执行文件内被解析。
4. **配置**：`KernelDebugBuild=ON`（需要 `seL4_DebugPutChar`/`DebugHalt`）、`KernelMaxNumNodes=1`、
   `LibSel4MuslSysCSpaceSize`/Untyped 足够大（LKL 会动态建多个 TCB）。
5. **LKL 与 muslc 的符号冲突**：LKL 内核符号（`printk`、`kmalloc` 等）与 muslc/musl 用户态可能同名。
   链接时给 LKL 内核对象加 `--prefix`/`-Wl,--wrap` 或用 LKL 的 `lkl_` 前缀导出，避免污染 muslc 命名空间。
   （LKL 默认编译已用 `lkl_`/静态符号域隔离内核符号，确认 `CONFIG_LKL` 相关隔离开关打开即可。）

CMake 骨架见 `CMakeLists.txt` 与 `apps/lkl-root-task/CMakeLists.txt`。

---

## 5. 最大阻碍：能力模型 vs LKL 的运行时动态性

**阻碍**：seL4 是 capability-based —— 创建 TCB、Notification、Endpoint、映射栈页都需要**显式持有并放置
cap**到 CSlot，且这些 cap 来自有限 Untyped/CSlot 预算。而 LKL 是单地址空间 + 软件上下文切换的 library OS，
它在**运行时**会动态 `thread_create`（boot CPU、timer、ksoftirqd、per-kthread…）、动态 `malloc`、动态 `timer_arm`。
一个被沙箱化的子任务若只拿到“固定小 CSpace + 小块 Untyped”，很快会**耗尽 CSlot/Untyped 而无法继续**，
而 seL4 没有隐式内核分配器可兜底 —— 一旦耗尽，`thread_create` 直接失败，LKL 内核 panic。
此外，LKL 假定 `malloc`/`pthread`“开箱即用”，与 seL4 “一切皆 cap”的根本模型冲突。

**绕过方法（按阶段）**：

1. **Phase 1（本项目采用）：LKL 与 root task 同址运行**。LKL 共享 root 的大 CSpace 与全部 Untyped，
   `lkl_host_ops` 经 `vka` 自由分配。代价：LKL 与 root task 无隔离（Phase 1 只验证“能跑起来”，可接受）。
2. **后续沙箱化：TCB 池 + cap-server**。在 root 预分配一组 TCB/栈（LKL 线程数小且可估，如 8–16），
   `thread_create` 向 root 的 cap-server RPC 取一个已就绪的 TCB cap，而非临时 `vka_alloc_tcb`。
   这样 cap 预算确定、可控，且支持限额。
3. **TLS 自举**：每个派生 TCB 必须配 TLS base + 拷贝 TLS 模板（§1.4），否则 LKL 访问 `current` 即崩。
   这是“能力模型”之外、但同样由 seL4 不自动给线程配 TLS 而起的坑。
4. **无需硬抢占**：定时器由独立 timer TCB 以软件事件投递（§1.5），CPU 线程在调度点处理，**不需要**
   MCS scheduling-context 来抢占 —— 规避了“跨 TCB 抢占需要 sc cap”的另一道能力门槛。
5. **console 的 debug 依赖**：`seL4_DebugPutChar` 仅 debug kernel 可用；上 release 必须接串口驱动
   （额外 IRQ cap + device untyped），属同类“显式 cap”问题，但属可解决项。

---

## 6. 里程碑与验证

- [ ] `liblkl.a` 交叉编译通过（seL4 用户态工具链）
- [ ] root task 启动，`lkl_host_ops.print` 能在串口打印（验证 console + boot）
- [ ] `lkl_start_kernel` 返回 0，boot CPU TCB 跑起来（验证 thread/timer/mutex/sem/tls 全链路）
- [ ] initramfs 内 `/init`（静态 hello）执行，串口出现 `hello from LKL on seL4`（验证 execve + syscall shim）
- [ ] `lkl_sys_halt` 干净退出

成功标志：串口一次性输出 `hello from LKL on seL4`，即证明 LKL 已作为 seL4 纯用户态 Task 完成基础初始化与运行。
