# luna — LKL on seL4（独立 task 隔离）

把 **LKL (Linux Kernel Library)** 作为 seL4 的**纯用户态 Task**跑起来，引导 Linux 内核，
并进入一个**交互式 shell**（操作 LKL 文件系统）。**不涉及 UINTR**。

**已验证**：QEMU x86_64 上，seL4 启动 → root manager 创建具有独立 CSpace/VSpace 的
`luna-lkl-task` → child 独立引导 LKL（Linux 6.12.0+）→ 进入交互 shell。`ls / cat / cd / mkdir /
write / stat / free` 等命令经 `lkl_sys_*` 跑在 child 内的 LKL 内核上，`free` 读到真实
`/proc/meminfo`。child 同时通过了真实单调时钟、oneshot timer、100ms `nanosleep`、固定资源池、
故障诊断、销毁重建和无 fault 关机验证。root manager 不再链接或运行 LKL。详见
[`PHASE2.1-RESULTS.md`](PHASE2.1-RESULTS.md)。

## 关键结论（核对 lkl/linux 源码后）

- LKL 的 init 进程**就是调用 `lkl_start_kernel` 的宿主线程**（`arch/lkl/kernel/setup.c:lkl_run_init`
  binfmt 仅 `sem_up(init_sem)+thread_exit()`）。**不需要 initramfs / virtio / 独立 ELF**。
  当前该宿主线程位于隔离的 `luna-lkl-task`，不是 root manager。
- API：`lkl_init(&host_ops)` + `lkl_start_kernel("mem=16M ...")`。
- `liblkl.a` 中只链接干净的内核对象 `lkl.o`（未定义符号仅 `lkl_printf`/`lkl_bug`，本仓提供）；
  不链接 POSIX host 胶水 `liblkl-in.o`。

## 目录结构

```
luna/
├── PHASE1-RESULTS.md            # Phase 1.1 历史结果
├── PHASE1.2-RESULTS.md          # 当前稳定性实现与验证
├── PHASE2.1-RESULTS.md          # 独立 task 隔离与渐进迁移记录
├── next-plan.md                 # 下一阶段规划与验收门槛
├── DESIGN.md                    # 早期设计（部分假设已被 PHASE1-RESULTS 纠正）
├── README.md
├── EVIDENCE_sel4_helloworld_boot.txt   # seL4 基础链路证据
├── EVIDENCE_lkl_on_sel4_boot.txt       # LKL-on-seL4 启动证据
├── apps/lkl-root-task/
│   ├── CMakeLists.txt           # 分别构建 root manager 与 luna-lkl-task
│   ├── include/                 # child host 与 manager/child 控制协议
│   └── src/
│       ├── main.c               # manager bootstrap、TSC 校准与 child 生命周期
│       ├── isolation_manager.c  # child 装载、fault 诊断、销毁与重建
│       ├── isolation_child.c    # luna-lkl-task 入口与控制协议
│       ├── lkl_task_host.c      # child 内的 LKL host operations
│       └── shell.c              # child 内的 LKL tty shell
├── build-artifacts/lkl-kernel.o # 从 liblkl.a 抽取的干净内核对象
├── setup-deps.sh                # 固定版本依赖拉取、补丁应用与环境检查
├── tools/smoke-test.sh          # 自动驱动 QEMU shell 的回归测试
└── deps/                        # seL4 manifest 源树 + lkl-linux（见 PHASE1-RESULTS 复现步骤）
    └── lkl_settings.cmake       # 非-tutorial 的 seL4 构建设置（x86_64 pc99, debug putchar）
```

## child host_ops 适配

| LKL host op | seL4 后端 |
|---|---|
| 内存（mem/page/mmap/shmem） | 静态 BSS bump 堆（避开派生线程 musl errno 缺页） |
| 线程 | manager 预建的固定 TCB 池（全部配置到 child VSpace/CSpace） |
| mutex/sem | Notification 二值信号量 |
| TLS | `[tid][key]` 表 + `__thread current_tid`（根线程 tid=1） |
| jmp_buf | musl setjmp/longjmp |
| timer/time | PIT 校准 TSC 单调时钟 + seL4 polling TCB oneshot（不占用冲突 IRQ） |
| console | 输出走 `seL4_DebugPutChar`；输入只使用 `0x3f8–0x3ff` COM1 capability |

## 复现

首次克隆后执行：

```sh
./setup-deps.sh
./run.sh --build-only
./tools/smoke-test.sh
```

`setup-deps.sh --check-only` 可审计已有依赖。固定版本、Python 模块和手工命令见
[`DEPENDENCIES.md`](DEPENDENCIES.md)。GitHub Actions 也执行同一套构建和 QEMU smoke test。

`run.sh` 默认给 QEMU 配置 512MiB 内存，root allocman metadata pool 为 4MiB，以装载 child ELF
的数千个 frame/mapping；QEMU 内存可通过 `LUNA_QEMU_MEM` 覆盖。

## 交互 shell 用法（经 LKL 虚拟串口 /dev/ttyLKL0）

```sh
./run.sh --no-timeout        # 交互模式（不超时），等 "lkl:/# " 提示符后键入命令
```

LKL 内置一个虚拟串口驱动 `arch/lkl/kernel/lkl_tty.c`（`/dev/ttyLKL0`，major 240）：
- **输出**：tty write → `lkl_ops->print` → `seL4_DebugPutChar`。
- **输入**：seL4 侧独立线程轮询 COM1 → 填 SPSC 环 → `lkl_trigger_irq()` 注入 IRQ → LKL IRQ handler
  从环取字符 push 进 tty flip 缓冲；n_tty ldisc 自动**回显 + 行编辑**。
- shell 经 `lkl_sys_read(0)`/`lkl_sys_write(1)` 走该 tty（fd 0/1/2 由 child 打开 `/dev/ttyLKL0` 设置）。

内置命令（经 `lkl_sys_*` 跑在 LKL 内核上）：`ls [path]`、`cat <f>`、`cd [path]`、`pwd`、
`mkdir <d>`、`rmdir <d>`、`rm <f>`、`touch <f>`、`write <f> <text…>`、`stat <f>`、`echo`、
`mount <src> <dir> <fstype>`、`free`（读 `/proc/meminfo`）、`sleep <ms>`、`time`、`help`、`exit`。

> 注：`run.sh` 先生成 LKL 构建配置，再只构建 `tools/lkl/liblkl.a`（含 lkl_tty 驱动）并重新
> 抽取 lkl.o，不链接 luna 不需要的 LKL 测试程序和 hijack 库。
> `exit` 会停止 child 控制台输入并执行 `lkl_sys_halt()`；manager 随后删除 child CSpace 中的派生
> capability、销毁 child 资源，打印
> `LUNA_SHUTDOWN_OK`。交互模式下用 Ctrl-A X 退出 QEMU。

完整 busybox sh 仍需任务 9/11（静态 lkl syscall shim + clone/fork），但**控制台 I/O 已经由 LKL tty 解决**。

## 状态

Phase 2.1 已完成。thread、sem/mutex、TLS、memory、time、oneshot timer、tty 和 shell host 能力均在
child。manager 为每个 child 预建 65 个 TCB slot（63 个 LKL 线程 + timer + console）和 128 个
同步 Notification，并只授予 child `0x3f8–0x3ff` 的 COM1 I/O-port capability。clean child 使用
32MiB host heap，以 `mem=16M` 启动内核，自动 smoke 的所有命令都在其独立 VSpace 内执行；fault
child 对 manager 动态映射在 `0x40000000` 的私有 probe 访问会产生预期 VMFault，销毁后 replacement
child 仍能完整 boot、交互和 halt。root ELF 不含 LKL 符号，只保留资源分配、TSC 校准、fault 诊断
和生命周期管理。

当前限制包括：child bump heap 不回收、mutex/sem 为二值，以及尚无 virtio 块设备/网络。
后续工作见 [`next-plan.md`](next-plan.md)。
