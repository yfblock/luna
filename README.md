# luna — LKL on seL4 (Phase 1.2 稳定基线) ✅

把 **LKL (Linux Kernel Library)** 作为 seL4 的**纯用户态 Task**跑起来，引导 Linux 内核，
并进入一个**交互式 shell**（操作 LKL 文件系统）。**不涉及 UINTR**。

**已验证**：QEMU x86_64 上，seL4 启动 → root task 引导 LKL（Linux 6.12.0+）→ `lkl_start_kernel`
返回 0（root task 成为 init/PID1）→ 进入交互 shell（`ls / cat / cd / mkdir / write / stat / free` 等经
`lkl_sys_*` 跑在 LKL 内核上，`free` 读到真实 `/proc/meminfo`）。Phase 1.2 进一步验证了真实单调时钟、
oneshot timer、`nanosleep`、累计线程槽复用和无 fault 关机。详见
[`PHASE1.2-RESULTS.md`](PHASE1.2-RESULTS.md)。

## 关键结论（核对 lkl/linux 源码后）

- LKL 的 init 进程**就是调用 `lkl_start_kernel` 的宿主线程**（`arch/lkl/kernel/setup.c:lkl_run_init`
  binfmt 仅 `sem_up(init_sem)+thread_exit()`）。**不需要 initramfs / virtio / 独立 ELF**。
  ⇒ Phase 1 的“用户态任务”即 seL4 root task：`lkl_start_kernel` 返回后它是 init，直接经 LKL 控制台打印 hello。
- API：`lkl_init(&host_ops)` + `lkl_start_kernel("mem=16M ...")`。
- `liblkl.a` 中只链接干净的内核对象 `lkl.o`（未定义符号仅 `lkl_printf`/`lkl_bug`，本仓提供）；
  不链接 POSIX host 胶水 `liblkl-in.o`。

## 目录结构

```
luna/
├── PHASE1-RESULTS.md            # Phase 1.1 历史结果
├── PHASE1.2-RESULTS.md          # 当前稳定性实现与验证
├── next-plan.md                 # 下一阶段规划与验收门槛
├── DESIGN.md                    # 早期设计（部分假设已被 PHASE1-RESULTS 纠正）
├── README.md
├── EVIDENCE_sel4_helloworld_boot.txt   # seL4 基础链路证据
├── EVIDENCE_lkl_on_sel4_boot.txt       # LKL-on-seL4 启动证据
├── apps/lkl-root-task/
│   ├── CMakeLists.txt           # seL4 root task（链 lkl-kernel.o + seL4 用户态库）
│   ├── include/sel4_lkl_host.h
│   └── src/
│       ├── main.c               # root task：bootstrap vka/vspace → lkl_init → lkl_start_kernel → 交互 shell
│       ├── shell.c              # 宿主侧交互 shell（COM1 轮询输入 + lkl_sys_* 命令）
│       └── lkl_host_ops.c       # struct lkl_host_operations 的 seL4 后端（核心）
├── build-artifacts/lkl-kernel.o # 从 liblkl.a 抽取的干净内核对象
├── setup-deps.sh                # 固定版本依赖拉取、补丁应用与环境检查
├── tools/smoke-test.sh          # 自动驱动 QEMU shell 的回归测试
└── deps/                        # seL4 manifest 源树 + lkl-linux（见 PHASE1-RESULTS 复现步骤）
    └── lkl_settings.cmake       # 非-tutorial 的 seL4 构建设置（x86_64 pc99, debug putchar）
```

## host_ops 适配（要点，详见 PHASE1-RESULTS）

| LKL host op | seL4 后端 |
|---|---|
| 内存（mem/page/mmap/shmem） | 静态 BSS bump 堆（避开派生线程 musl errno 缺页） |
| 线程 | sel4utils TCB（共享 VSpace/CSpace，自动建 TLS） |
| mutex/sem | Notification 二值信号量 |
| TLS | `[tid][key]` 表 + `__thread current_tid`（根线程 tid=1） |
| jmp_buf | musl setjmp/longjmp |
| timer/time | PIT 校准 TSC 单调时钟 + seL4 polling TCB oneshot（不占用冲突 IRQ） |
| console | `seL4_DebugPutChar` |

## 复现

首次克隆后执行：

```sh
./setup-deps.sh
./run.sh --build-only
./tools/smoke-test.sh
```

`setup-deps.sh --check-only` 可审计已有依赖。固定版本、Python 模块和手工命令见
[`DEPENDENCIES.md`](DEPENDENCIES.md)。GitHub Actions 也执行同一套构建和 QEMU smoke test。

## 交互 shell 用法（经 LKL 虚拟串口 /dev/ttyLKL0）

```sh
./run.sh --no-timeout        # 交互模式（不超时），等 "lkl:/# " 提示符后键入命令
```

LKL 内置一个虚拟串口驱动 `arch/lkl/kernel/lkl_tty.c`（`/dev/ttyLKL0`，major 240）：
- **输出**：tty write → `lkl_ops->print` → `seL4_DebugPutChar`。
- **输入**：seL4 侧独立线程轮询 COM1 → 填 SPSC 环 → `lkl_trigger_irq()` 注入 IRQ → LKL IRQ handler
  从环取字符 push 进 tty flip 缓冲；n_tty ldisc 自动**回显 + 行编辑**。
- shell 经 `lkl_sys_read(0)`/`lkl_sys_write(1)` 走该 tty（fd 0/1/2 由 root task 打开 /dev/ttyLKL0 设置）。

内置命令（经 `lkl_sys_*` 跑在 LKL 内核上）：`ls [path]`、`cat <f>`、`cd [path]`、`pwd`、
`mkdir <d>`、`rmdir <d>`、`rm <f>`、`touch <f>`、`write <f> <text…>`、`stat <f>`、`echo`、
`mount <src> <dir> <fstype>`、`free`（读 `/proc/meminfo`）、`sleep <ms>`、`time`、`help`、`exit`。

> 注：`run.sh` 会 `make -C deps/lkl-linux/tools/lkl` 重建 LKL（含 lkl_tty 驱动）并重新抽取 lkl.o。
> `exit` 会停止控制台输入、执行 `lkl_sys_halt()` 和 host 清理，打印 `LUNA_SHUTDOWN_OK` 后将
> root task 明确挂起。交互模式下用 Ctrl-A X 退出 QEMU。

完整 busybox sh 仍需任务 9/11（静态 lkl syscall shim + clone/fork），但**控制台 I/O 已经由 LKL tty 解决**。

## 状态

Phase 1.2 **完成并自动验证**：LKL 启动、tty shell、单调时间、oneshot timer、100ms sleep、
80 次累计线程创建/回收、文件系统命令和无 fault 关机均纳入 smoke test。

当前限制仍包括：bump heap 不回收、mutex/sem 为二值、无 virtio 块设备/网络、LKL 与 root task
同址且没有 capability 隔离。后续工作见 [`next-plan.md`](next-plan.md)。
