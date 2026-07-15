# luna — LKL on seL4（独立 task 隔离）

把 **LKL (Linux Kernel Library)** 作为 seL4 的**纯用户态 Task**跑起来，引导 Linux 内核，
并进入一个**交互式 shell**（操作 LKL 文件系统）。**不涉及 UINTR**。

**已验证**：QEMU x86_64 上，seL4 启动 → root manager 创建具有独立 CSpace/VSpace 的
`luna-lkl-task` → child 独立引导 LKL（Linux 6.12.0+）→ 进入交互 shell。`ls / cat / cd / mkdir /
write / stat / free` 等命令经 `lkl_sys_*` 跑在 child 内的 LKL 内核上，`free` 读到真实
`/proc/meminfo`。child 同时通过了真实单调时钟、oneshot timer、100ms `nanosleep`、固定资源池、
故障诊断、销毁重建和无 fault 关机验证。Phase 2.2 又加入 manager 受控的可回收页映射、完整的
同步/TLS/thread/timer 生命周期语义和连续 100 次 child boot/halt/destroy 压力测试。Phase 2.3 又加入
manager 私有 backing 的 virtio-block、ext4 持久根文件系统和跨 100 次 child 重建的数据校验。
Phase 2.4 又加入静态 host-program ABI，并从持久 rootfs 启动最小 BusyBox `ash`/`cat`，通过专用
program thread 执行、退出和 join。Phase 2.5 又加入 manager-owned QEMU virtio-net、固定 IPv4、
外部 ICMP/TCP echo 与网络窗回收。
Phase 2.5.1 又将 RX 改为 receive-only Notification 异步唤醒，并加入 bounded queue 背压、丢包统计
和 64×1200-byte UDP burst 回归。root manager 不再链接或运行 LKL。详见
[`PHASE2.1-RESULTS.md`](PHASE2.1-RESULTS.md) 和
[`PHASE2.2-RESULTS.md`](PHASE2.2-RESULTS.md)、
[`PHASE2.3-RESULTS.md`](PHASE2.3-RESULTS.md)、
[`PHASE2.4-RESULTS.md`](PHASE2.4-RESULTS.md)、
[`PHASE2.5-RESULTS.md`](PHASE2.5-RESULTS.md) 和
[`PHASE2.5.1-RESULTS.md`](PHASE2.5.1-RESULTS.md)。

## 关键结论（核对 lkl/linux 源码后）

- LKL 的 init 进程**就是调用 `lkl_start_kernel` 的宿主线程**（`arch/lkl/kernel/setup.c:lkl_run_init`
  binfmt 仅 `sem_up(init_sem)+thread_exit()`）。**不需要 initramfs / virtio / 独立 ELF**。
  当前该宿主线程位于隔离的 `luna-lkl-task`，不是 root manager。
- API：`lkl_init(&host_ops)` + `lkl_start_kernel("mem=16M ...")`。
- `liblkl.a` 中只链接干净的内核对象 `lkl.o`（未定义符号仅 `lkl_printf`/`lkl_bug`，本仓提供）；
  不链接 POSIX host 胶水 `liblkl-in.o`。
- 当前 LKL 是 NOMMU host-call 内核，`elf_check_arch()` 拒绝 ELF 且 `start_thread()` 为空。BusyBox
  因此使用 Luna 静态 host-program ABI，而不是虚构为 LKL 内部的 Linux 用户进程。

## 目录结构

```
luna/
├── PHASE1-RESULTS.md            # Phase 1.1 历史结果
├── PHASE1.2-RESULTS.md          # 当前稳定性实现与验证
├── PHASE2.1-RESULTS.md          # 独立 task 隔离与渐进迁移记录
├── PHASE2.2-RESULTS.md          # 可回收页映射与 100 轮生命周期压力结果
├── PHASE2.3-RESULTS.md          # virtio-block、ext4 根与持久化结果
├── PHASE2.4-RESULTS.md          # 静态程序 ABI、BusyBox 与 spawn/wait 结果
├── PHASE2.5-RESULTS.md          # virtio-net、IPv4、ICMP/TCP 与回收结果
├── PHASE2.5.1-RESULTS.md        # 异步 RX、背压、统计与 burst 压力结果
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
│       ├── lkl_task_disk.c      # virtio-blk、mount/chroot 与设备生命周期
│       ├── isolation_net.c      # manager PCI/virtio ethdriver、DMA 与 bounded queue
│       ├── lkl_task_net.c       # child LKL virtio-net backend、IPv4 与网络 smoke
│       ├── lkl_task_user.c      # BusyBox 静态 ABI、LKL syscall shim 与 program launcher
│       └── shell.c              # child 内的 LKL tty shell
│   └── rootfs/                  # ext4 seed rootfs
├── build-artifacts/lkl-kernel.o # 从 liblkl.a 抽取的干净内核对象
├── setup-deps.sh                # 固定版本依赖拉取、补丁应用与环境检查
├── tools/smoke-test.sh          # 自动驱动 QEMU shell 的回归测试
├── tools/make-rootfs.sh         # 生成并 e2fsck 校验 ext4 镜像
├── tools/pack-rootfs.py         # 稀疏打包非零 rootfs block
├── tools/build-busybox-object.sh # 构建并重定向最小 BusyBox 对象
├── tools/net-peer.py            # QEMU socket netdev 的外部 ARP/ICMP/TCP 对端
└── deps/                        # seL4 manifest、lkl-linux 与 BusyBox
    └── lkl_settings.cmake       # 非-tutorial 的 seL4 构建设置（x86_64 pc99, debug putchar）
```

## child host_ops 适配

| LKL host op | seL4 后端 |
|---|---|
| 内存（mem/page/mmap/shmem） | child 位图分配器 + manager 按需 map/unmap；child 不持有 VKA/Untyped |
| 线程 | manager 预建的固定 TCB 池；唯一递增 tid、单 join claim、显式 exit 与安全 slot reuse |
| mutex/sem | 原子计数 semaphore + Notification 唤醒；mutex 记录 owner/递归深度并拒绝非 owner 解锁 |
| TLS | `[slot][key]` 表 + 最多 4 轮可重入 destructor（与可复用 slot 绑定，不复用公开 tid） |
| jmp_buf | musl setjmp/longjmp |
| timer/time | PIT 校准 TSC 单调时钟 + generation oneshot；cancel barrier 与 rearm 失效旧 deadline |
| block | LKL virtio-mmio/blk + 64KiB IPC 传输窗；16MiB backing 仅由 manager 持有 |
| static program | BusyBox `lbb_main` + LKL fd shim + 1MiB bounded arena + host thread spawn/join |
| network | manager poll thread + receive-only Notification + 32-entry RX queue + 8KiB transfer window |
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

`run.sh` 默认给 QEMU 配置 512MiB 内存，root allocman metadata pool 为 8MiB，以装载 child ELF、
重建 ext4 backing 并维护数千个 frame/mapping；QEMU 内存可通过 `LUNA_QEMU_MEM` 覆盖。
默认 smoke 包含 100 轮完整重启与 ext4 重挂载压力测试，超时上限为 480 秒。
`run.sh` 还会启动 localhost UDP Ethernet peer，并固定创建 BDF `00:05.0` 的 legacy
virtio-net-pci；不需要 root/TAP，也不依赖 QEMU slirp。

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
`mount <src> <dir> <fstype>`、`sync`、`free`（读 `/proc/meminfo`）、`sleep <ms>`、`time`、`help`、
`exit`。clean child 的 shell 已 chroot 到 ext4，提示符中的 `/` 是持久根文件系统。

> 注：`run.sh` 先生成 LKL 构建配置，再只构建 `tools/lkl/liblkl.a`（含 lkl_tty 驱动）并重新
> 抽取 lkl.o，不链接 luna 不需要的 LKL 测试程序和 hijack 库。BusyBox 只构建 `ash/echo/cat` 的
> relocatable host-program 对象，不使用 LD_PRELOAD hijack。
> `exit` 会停止 child 控制台输入并执行 `lkl_sys_halt()`；manager 随后删除 child CSpace 中的派生
> capability、销毁 child 资源，打印
> `LUNA_SHUTDOWN_OK`。交互模式下用 Ctrl-A X 退出 QEMU。

clean child 在开放交互 shell 前自动执行 BusyBox 验收命令：
`sh -c 'echo ok > /tmp/x; cat /tmp/x'`。其 fd 0/1/2 与文件重定向均进入 LKL。

## 状态

Phase 2.1 已完成。thread、sem/mutex、TLS、memory、time、oneshot timer、tty 和 shell host 能力均在
child。manager 为每个 child 预建 65 个 TCB slot（63 个 LKL 线程 + timer + console）和 128 个
同步 Notification，并只授予 child `0x3f8–0x3ff` 的 COM1 I/O-port capability。clean child 使用
`mem=16M` 启动内核，自动 smoke 的所有命令都在其独立 VSpace 内执行；fault
child 对 manager 动态映射在 `0x40000000` 的私有 probe 访问会产生预期 VMFault，销毁后 replacement
child 仍能完整 boot、交互和 halt。root ELF 不含 LKL 符号，只保留资源分配、TSC 校准、fault 诊断
和生命周期管理。

Phase 2.2 的内存与重启部分也已完成。child 在 `0x20000000` 保留 32MiB 虚拟 arena，但物理页只在
host operation 请求时由 manager 映射，释放时立即 unmap 并归还 VKA。child ELF 不再携带 32MiB
BSS heap；allocator 自测覆盖全 arena、释放复用和清零，manager 在每次 halt 后断言映射页计数为 0，
并连续完成 100 轮 LKL 启动/关闭/销毁。

Phase 2.2 已完整完成。semaphore 保存真实 token 计数并支持多个阻塞 waiter，mutex 拒绝非 owner
解锁并验证 recursive depth；TLS destructor 在重新设置 key 时最多重试 4 轮。thread 使用唯一 tid、
单 join claim 和 table lock 防止 slot reuse ABA；timer 使用 generation、cancel barrier 和 callback/free
排序保证 rearm 后旧 deadline 不触发、free 返回后旧 callback 不再执行。以上自测在每个 stress child
中执行，最终 100 轮 QEMU smoke 全部通过。

Phase 2.3 已完成。LKL 通过 virtio-mmio 探测 `vda`，manager 只向 child 映射 64KiB 传输窗，完整
16MiB backing 和 frame cap 留在 manager。fault child 写入的 ext4 marker 经 100 个 stress child 的
完整 boot/mount/unmount/halt/destroy 后仍可读取，最终 clean shell 以该 ext4 为 `/`。构建阶段生成
固定 rootfs、运行 `e2fsck -fn` 并只嵌入非零 block 的稀疏 pack。

Phase 2.4 已完成。持久 rootfs 中的 `/bin/busybox` 选择静态 ABI v1，child 通过 BusyBox `lbb_main()`
运行最小 `ash`、builtin `echo` 与 nofork `cat`。program thread 使用固定 TCB pool，退出后完成 join、
TLS cleanup 和 slot reuse；自动测试验证 `/tmp/x` 内容为 `ok\n`。native `fork/exec` 在 LKL NOMMU
arch 中不可用并被 shim 显式拒绝，完整边界见 `PHASE2.4-RESULTS.md`。

Phase 2.5 已完成。manager 独占 QEMU virtio-net-pci 的 PCI I/O 与 DMA，child 只映射独立 TX/RX
两页网络窗并通过受控 Endpoint 收发。LKL 配置 `10.0.2.15/24`，自动测试跨 QEMU socket backend
与独立 host peer 完成 ARP、ICMP echo 和 TCP payload echo；100 轮 child 重建继续验证网络 frame cap
和 mapping 回收。完整边界见 `PHASE2.5-RESULTS.md`。

Phase 2.5.1 已完成。manager 的独立 RX thread 是 ethdriver `raw_poll()` 的唯一调用者；child LKL
poll thread 无包时阻塞在 receive-only Notification，不再循环发送 Endpoint 请求。32-entry bounded
queue 记录 high-water、backpressure、drop 和 empty-fetch，自动测试通过 64 个 1200-byte UDP packet
制造慢消费者并验证队列恢复。完整结果见 `PHASE2.5.1-RESULTS.md`。

当前限制包括：小于一页的 host allocation 仍按页计费、manager RAM backing 不跨整个 QEMU 电源周期，
静态 ABI 尚不支持 pipeline/native fork/exec，manager 物理网卡侧仍使用 polling thread 而非 IRQ，且默认测试对端
是确定性的 localhost 二层 peer，不是互联网出口。
后续工作见 [`next-plan.md`](next-plan.md)。
