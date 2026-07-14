# Phase 1.1 — 实际实现与验证结果（已跑通）

> 本文保留 Phase 1.1 的历史实现和踩坑记录。timer、线程复用、fault endpoint、关机路径与
> 自动化测试已经在 Phase 1.2 中更新，当前状态以 [`PHASE1.2-RESULTS.md`](PHASE1.2-RESULTS.md)
> 为准。

本文件记录 Phase 1.1 **实际**落地并经 QEMU 验证的结果，与早期 `DESIGN.md` 中基于假设的方案
（initramfs + execve 静态 ELF + virtio）不同——核对真实 `lkl/linux` 源码后，那些假设不成立。
以下为已验证事实。

## 验证证据

`EVIDENCE_lkl_on_sel4_boot.txt` 是 QEMU 实跑串口日志（已去 ANSI）。关键行：

```
luna: lkl_init ok
luna: calling lkl_start_kernel...
[    0.000000] Linux version 6.12.0+ (yfblock@9950x) (gcc 13.3.0) #1 ...
[    0.000000] memblock address range: 0x10d2000 - 0x20d2000
[    0.000000] Kernel command line:  mem=16M loglevel=8
[    0.000000] lkl: irqs initialized
[    0.000000] lkl: time and timers initialized (irq1)
[    0.000000] SLUB: HWalign=32 ... CPUs=1 Nodes=1
[    0.000000] printk: legacy console [lkl_console0] enabled
[    0.000000] NET: Registered PF_INET ...
[    0.000000] Run /init as init process
luna: lkl_start_kernel returned 0
hello from LKL on seL4
luna: hello emitted via LKL console
```

`EVIDENCE_sel4_helloworld_boot.txt` 是 seL4 基础链路（root task + DebugPutChar）先期跑通的证据。

## 关键事实（核对源码后纠正早期假设）

1. **LKL 的“init 进程”就是调用 `lkl_start_kernel` 的宿主线程本身。**
   `arch/lkl/kernel/setup.c` 的 `lkl_run_init`（binfmt `lkl_run_init_binfmt`）只对文件名 `/init`
   做 `begin_new_exec` 后 `sem_up(init_sem) + thread_exit()`，随后 `lkl_start_kernel` 返回——
   **不加载/执行任何独立 ELF**，不需要 initramfs、不需要 virtio。
   ⇒ Phase 1 的“用户态任务”即 seL4 root task：`lkl_start_kernel` 返回后它就是 init/PID1，
   直接经 LKL 控制台打印 hello。

2. **API 是 `lkl_init(ops)` + `lkl_start_kernel(cmdline)`**（不是 `lkl_start_kernel(ops, mem, ...)`）。
   host_ops 经 `lkl_init` 注入到 lkl.o 内 hidden 的 `lkl_ops` 全局。

3. **`liblkl.a` 中只有 `lkl.o`（内核）干净**：未定义符号仅 `lkl_printf` / `lkl_bug`（+ GOT）。
   `liblkl-in.o`（POSIX host + virtio 胶水）**不链接**——我们用本仓 `lkl_host_ops.c` 取代。
   故无需对 pthread / socket / timer_create / glibc `_chk` 做任何 stub。

4. **LKL `__pa`/`__va` 为恒等映射**（`arch/lkl/include/asm/page-mmu.h`），故 `shmem_mmap`
   返回任意有效指针即可，不必映射到 `lkl_va_base`。`bootmem_init` 用 `shmem_mmap` 取
   `mem=` 字节的“物理内存”。

## 实际实现的 host_ops（`apps/lkl-root-task/src/lkl_host_ops.c`）

| 项 | 实现 |
|---|---|
| `mem_alloc`/`page_alloc`/`mmap`/`shmem_mmap` | **静态 BSS bump 堆**（32 MiB `g_heap_buf`，loader 映射，绕过 vspace 限额；不回收） |
| `mem_free`/`munmap`/`page_free` | no-op（bump） |
| `memcpy`/`memset`/`memmove` | musl |
| `thread_create` | `sel4utils_configure_thread_config`（共享 root VSpace/CSpace，256 KiB 栈）+ `sel4utils_start_thread`，trampoline 设 `__thread current_tid` |
| `thread_join` / `thread_exit` | 每 TCB 一个 join Notification；exit=signal+Suspend，join=Recv+clean_up |
| `thread_self` | `__thread current_tid`；**根线程 tid=1**（LKL 约定 0=无线程，`lkl_cpu_put` 在 owner==0 时 panic） |
| `mutex_*` | Notification 二值信号量 + owner/count（支持 recursive） |
| `sem_*` | Notification 二值信号量（Signal/Recv，latch 不丢唤醒；count>1 塌缩，LKL 实际 0/1） |
| `tls_*` | `[tid][key]` 表，`struct lkl_tls_key *` 句柄 |
| `jmp_buf_set`/`jmp_buf_longjmp` | musl `setjmp`/`longjmp`，`lkl_jmp_buf.buf[128]` 当 `jmp_buf` 用 |
| `timer_*` | **stub**（pc99 默认 ltimer 与内核已占用的 IRQ 冲突“IRQ 16 already active”；boot→init 路径同步，无需 tick） |
| `time` | `ltimer_get_time`（timer 未初始化时返回 0） |
| `print` | `seL4_DebugPutChar` 循环（debug kernel） |
| `panic` | 打印 + `seL4_DebugHalt` + Suspend |
| `ioremap`/`iomem_access` | no-op（无 virtio） |
| `lkl_printf`/`lkl_bug` | `vsnprintf` + `seL4_lkl_host_ops.print/.panic`（lkl_ops 在 lkl.o 为 hidden，外部不可见） |

## 根任务（`apps/lkl-root-task/src/main.c`）

1. `platsupport_get_bootinfo` → `simple_default_init_bootinfo` → `bootstrap_use_current_simple`
   → `allocman_make_vka` → `sel4utils_bootstrap_vspace_with_bootinfo`。
2. `sel4_lkl_host_init`（fault ntfn + 根 tid=1 + bump 堆）。
3. `lkl_init(&seL4_lkl_host_ops)`。
4. `lkl_start_kernel("mem=16M loglevel=8")` → 内核在派生 TCB 上跑 `start_kernel` → `run_init_process("/init")`（lkl_run_init 空操作）→ `sem_up`+`thread_exit` → 本函数返回 0，宿主成为 init。
5. `lkl_printf("hello from LKL on seL4\n")` → `lkl_ops->print` → `seL4_DebugPutChar`。
6. `lkl_sys_halt()`。

## 路上踩过的坑（均已在代码中解决）

- **派生线程 musl `errno` (TLS) 访问缺页**：`seL4_TCB_SetTLSBase` 设置的 tp 使 `errno` 落在栈顶未映射页。
  根因是 host 的 `sem_alloc`/`mutex_alloc` 用了 musl `malloc` → `mmap` shim → `__syscall_ret` 写 `errno` → 缺页。
  解法：所有 host 分配改用静态 BSS bump 堆，派生线程绝不走 musl malloc。
- **根线程 tid=0**：`lkl_cpu_put` 在 `cpu.owner==0` 时 panic。解法：根线程 `current_tid=1`。
- **`vspace_new_pages` 大块映射在 ~15 MiB 处耗尽**（“Failed to allocate object of size 4096”）。
  解法：bump 堆改用 32 MiB 静态 BSS（loader 映射，不经 vspace）。
- **pc99 默认 ltimer IRQ 冲突**（“IRQ 16 already active” + allocman assert）。解法：Phase 1 timer 走 stub。
- **`lkl_ops` hidden**：lkl.o 内 `lkl_ops` 是 hidden 可见性；`lkl_printf`/`lkl_bug` 直接用本仓 `seL4_lkl_host_ops`。

## 复现

```sh
# 依赖（已在本仓 deps/ 下）：seL4 manifest(seL4_tools/util_libs/musllibc/sel4runtime/seL4_libs/
#        capdl/global-components/sel4-tutorials) + lkl/linux  fork
export PATH=~/bin:$PATH                      # repo 启动器 + xmllint stub（见下）
export LUNA_SETTINGS=$PWD/deps/lkl_settings.cmake
export LKL_LINUX_DIR=$PWD/deps/lkl-linux
export LKL_KERNEL_OBJ=$PWD/build-artifacts/lkl-kernel.o   # ar x liblkl.a lkl.o

# 1) 建 liblkl.a 并抽取干净内核对象 lkl.o
make -C deps/lkl-linux/tools/lkl conf
make -C deps/lkl-linux/tools/lkl -j "$PWD/deps/lkl-linux/tools/lkl/liblkl.a"
ar x deps/lkl-linux/tools/lkl/liblkl.a lkl.o && mv lkl.o build-artifacts/lkl-kernel.o

# 2) 配置 + 构建 seL4 root task（含 LKL）
cmake -G Ninja -B deps/build_lkl -S apps/lkl-root-task
ninja -C deps/build_lkl

# 3) QEMU 启动
cd deps/build_lkl && ./simulate -b $(command -v qemu-system-x86_64)
```

环境补丁：`xmllint` 缺失→放一个 exit-0 的 `~/bin/xmllint` stub（seL4 仅用它做已提交 XML 的 XSD 校验）；
seL4 capdl python 依赖 `sh aenum pyelftools sortedcontainers future jinja2`（`pip3 install --user --break-system-packages`）。

## 已知限制（Phase 1）

- timer 为 stub（无时钟中断）；halt 路径在 LKL 协作模型下触发 reboot 后有次生 fault（hello 已打印）。
- 控制台为 printk-console，无 tty 设备节点（`/dev/console` 返回 -ENODEV）；hello 经 `lkl_printf` 输出。
- mutex/sem 为二值；bump 堆不回收；无 virtio/块设备/网络数据通路（仅内核初始化）。
- LKL 与 root task 同址运行（无隔离），见 `DESIGN.md §5` 的沙箱化方向。
