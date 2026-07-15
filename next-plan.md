# luna 下一阶段规划

当前基线已经具备可复现构建、自动 QEMU 回归、交互 tty、真实单调时间、oneshot timer 和无 fault
关机。下一阶段不再继续堆叠 root task 内的演示功能，而应把它推进为有边界、可运行真实程序的系统。

## Phase 2.1：LKL 独立任务与 capability 隔离

目标：将 LKL 从 seL4 root task 拆成独立用户态 task，root task 只负责资源分配和生命周期管理。

主要工作：

- 为 LKL task 建立独立 VSpace、CSpace、TCB 和 fault endpoint。
- 只授予所需的 Notification、共享内存、串口或虚拟设备 capability。
- 定义 root manager 与 LKL task 的启动、控制、日志和关机协议。
- fault handler 能报告 task、地址、寄存器和 fault 类型，单个 LKL fault 不拖垮管理任务。

验收门槛：

- LKL task 无权访问 root manager 私有页和 capability。
- 人工触发 LKL fault 后，root manager 仍存活并能输出诊断或重启 LKL。
- 现有 smoke test 在隔离结构下继续通过。

完成状态：Phase 2.1 已完成。manager 能装载具有独立 CSpace/VSpace/TCB/fault endpoint 的
child；child 访问 manager 私有页会产生可诊断 VMFault，manager 随后能销毁并重建 child，且原有
LKL smoke 继续通过。manager→child receive-only 命令 Endpoint 和 child→manager 事件 Endpoint 已通过，
`lkl.o` 也已链接进 child 并报告 `LKL_LINKED`。Phase 2.1b/c 进一步建立 65 个固定 TCB slot、128 个
同步 Notification、32MiB child heap、timer TCB 和 console TCB；child 已独立完成 `lkl_init()`、
`lkl_start_kernel("mem=16M")`、真实 oneshot timer、tty shell 和 `lkl_sys_halt()`。manager 只复制
`0x3f8–0x3ff` COM1 capability，自动 shell smoke 的命令已全部在 child VSpace 内执行。fault child
销毁后 replacement child 能再次完整 boot、交互并 halt。
root target 已不再链接 `lkl.o`、旧 host ops 或 shell；manager 独立通过 PIT 校准 TSC，隔离 probe
也改为 `0x40000000` 的动态私有映射。root 现在只保留资源分配、fault 诊断和生命周期管理。
实现与证据见 `PHASE2.1-RESULTS.md`。

## Phase 2.2：可回收 host 资源与同步语义

目标：消除当前只适合单次启动的资源模型。

主要工作：

- [x] 用可回收页分配器替代 32MiB bump heap，记录映射和释放。
- [x] 实现真正可计数的 semaphore，并补齐 mutex owner 校验。
- [x] 扩展 TLS destructor 的多轮/重入语义测试，并处理 destructor 再次设置 key 的情况。
- [x] 增加并发创建、退出、join 和 timer cancel/rearm 压力测试。
- [x] 增加重复启动/关机压力测试。

验收门槛：连续启动/关机 LKL 100 次，无 capability、页映射或线程槽持续增长。

内存与重复生命周期部分已完成。child 只维护 8192 页位图和 allocation record，通过现有
事件/命令 Endpoint 请求 manager 在 `0x20000000` 的 32MiB arena 内按需 map/unmap；manager 对地址、
页数和重复映射进行校验，并在 allocator 自测与 LKL halt 后断言映射页计数为 0。child 仍未获得
VKA 或 Untyped。默认 smoke 已连续完成 100 次完整 LKL boot/halt/destroy，随后 replacement child
仍能运行 tty shell。实现与证据见 `PHASE2.2-RESULTS.md`。

同步、TLS、thread 与 timer 部分也已完成。自测覆盖 count=3 的 token 累计、4 个已阻塞 waiter 的逐一
唤醒、非 owner unlock 拒绝、recursive mutex depth，以及 destructor 重新设置同一 key 的 3 轮调用和
4 轮上限。thread 测试覆盖唯一 tid、两个 joiner claim 竞争、显式 exit、重复/旧 tid join 和 64 轮
slot reuse；timer 测试覆盖立即 cancel、连续 32 次 rearm 覆盖旧 deadline 和 16 轮 callback/free 竞争。

完成状态：Phase 2.2 已完整完成。最终构建和 100 轮 QEMU smoke 均通过。

## Phase 2.3：virtio-block 与持久根文件系统

目标：让 LKL 使用真实块设备镜像，而不只依赖内存文件系统。

主要工作：

- [x] 建立 seL4 host 侧 virtio-mmio/block backend。
- [x] 向 LKL 注入设备描述并完成 IRQ/共享队列连接。
- [x] 挂载 ext4 镜像作为持久文件系统并将 clean shell chroot 到该根。
- [x] 增加 managed restart、一致性检查和文件读写测试。

验收门槛：QEMU smoke test 能创建文件、正常销毁 LKL child、再次创建并读取同一文件，seed 镜像通过
`e2fsck -fn`。

完成状态：Phase 2.3 已完成。构建生成固定 UUID 的 16MiB ext4 并运行 `e2fsck -fn`；QEMU 通过
`memory-backend-file` 和 `ivshmem-plain` 将宿主文件映射给 manager，manager 只向 child 映射 64KiB
传输窗。fault child 写入持久 marker，100 个 replacement
stress child 均重新探测 `vda`、挂载并读取同一文件，最终 clean child 以该 ext4 为 `/` 运行 shell，
随后正常 sync、halt 和回收 host virtio 对象。实现与证据见 `PHASE2.3-RESULTS.md`。

Phase 2.3.1 已将持久化边界扩展到整个 QEMU 进程重启。`--cross-qemu` 回归使用同一个临时 ext4
文件启动两次 QEMU，第二次读回第一次写入的标记；详见 `PHASE2.3.1-RESULTS.md`。

## Phase 2.4：真实用户程序与 BusyBox

目标：从宿主内置命令升级为运行静态用户程序。

主要工作：

- [x] 明确 LKL 当前“宿主线程即 init”模型下的用户 ABI 边界。
- [x] 实现静态 syscall shim、程序启动参数和 fd 继承。
- [x] 用固定 host thread pool 实现静态 program spawn/join/exit 回收。
- [x] 在持久根文件系统中启动最小 BusyBox `ash/echo/cat`。

验收门槛：自动测试通过 BusyBox 执行 `sh -c 'echo ok > /tmp/x; cat /tmp/x'` 并正常回收子进程。

完成状态：Phase 2.4 已完成。BusyBox 1.36.1 以静态 host-program object 链接进隔离 child，rootfs
中的 `/bin/busybox` 是 `LUNA-STATIC-ABI-1` 描述。LKL fd shim 覆盖 shell 重定向所需接口，独立
program thread 执行 `lbb_main()`，`exit` 回到 launcher 后完成 join/TLS cleanup。该 LKL arch 为
NOMMU，`elf_check_arch()` 拒绝 ELF 且 `start_thread()` 为空，因此 native `fork/exec` 不属于可实现的
LKL 内部路径；shim 会显式拒绝这些调用。实现与证据见 `PHASE2.4-RESULTS.md`。

### Phase 2.4.1：可交互 BusyBox ash

- [x] 让 `ash -i` 直接接管 `/dev/ttyLKL0`，删除 Luna 自定义命令解释器。
- [x] 将 isatty/termios/sleep 重定向到 LKL。
- [x] 将 1MiB bump arena 改为支持 split/free/coalesce/realloc 的 bounded heap。
- [x] 自动交互验证 builtin、重定向、条件执行、nofork cat、sleep 与 `/proc`。
- [x] 保留 100 轮生命周期、网络 IRQ、ext4 FSCK 与跨 QEMU 持久化门槛。

完成状态：Phase 2.4.1 已完成。实现与证据见 `PHASE2.4.1-RESULTS.md`。

### Phase 2.4.2：LKL-aware 标准流 stdio

- [x] 将 printf/vprintf/dprintf 的 stdout 输出写入 LKL fd。
- [x] 将 stdout/stderr 的 fprintf、puts、putchar、fputs、putc 写入 LKL fd。
- [x] 支持标准流 fflush/ferror/clearerr，并拒绝未支持的任意 FILE stream。
- [x] 自动验证 `printf > file` 后由 `cat` 读回格式化内容。

完成状态：Phase 2.4.2 已完成。实现与证据见 `PHASE2.4.2-RESULTS.md`。

### Phase 2.4.3：BusyBox nofork applet 扩展

- [x] 启用 `mkdir`、`rmdir`、`unlink`、`truncate`、`basename`、`dirname`、`printenv`、`uname`。
- [x] 将 mkdir/rmdir/chmod/ftruncate/uname 转入 child 内 LKL syscall。
- [x] 在持久 ext4 上自动验证目录创建、文件截断与路径 applet。
- [x] 挂载易失 ramfs `/run` 验证删除语义，同时保留宿主 ext4 FSCK 门槛。
- [x] 保留 `forbidden=0`、100 轮生命周期、网络压力与跨 QEMU 持久化回归。

完成状态：Phase 2.4.3 已完成。实现与证据见 `PHASE2.4.3-RESULTS.md`。

### Phase 2.4.4：受控静态 process/pipe runtime

- [x] 构建 4 份符号隔离的 BusyBox nofork worker，并为每个 worker 隔离 libbb 全局状态。
- [x] 实现 worker 虚拟 fd 0/1/2、LKL `pipe2`、2–4 段 pipeline 和末段退出状态传播。
- [x] 实现静态后台 spawn、ash job 记录、SIGCHLD 通知、`waitpid` 与线程回收。
- [x] 为共享 1MiB heap 和 getopt 状态增加并发保护。
- [x] 实现 LKL-aware 任意文件 `FILE*` subset，验证 `head/wc/cut/uniq`。
- [x] 扩展 `touch/ln/readlink/realpath/echo/printf/true/false` 等常用 applet。
- [x] 保持 native ELF `fork/exec` 为明确 NOMMU 架构边界，不扩大 child capability。

完成状态：Phase 2.4.4 已完成。实现与证据见 `PHASE2.4.4-RESULTS.md`。

### Phase 2.4.5：常用 applet 与 runtime 负向回归

- [x] 启用并验证 `cmp/sort/grep/tee/dd/find/ls/cp/mv`。
- [x] 实现 LKL-aware 目录流、access/chown/mknod/utimes 与补充 stdio ABI。
- [x] 验证不存在命令、不支持/超长 pipeline 和 4-slot worker 耗尽。
- [x] 验证 4 个并发后台 job、SIGCHLD 与 `waitpid(-1)` 回收。

完成状态：Phase 2.4.5 已完成。实现与证据见 `PHASE2.6-RESULTS.md`。

## Phase 2.5：网络数据通路

目标：接通 LKL 内核网络栈与 seL4/QEMU 外部网络。

主要工作：virtio-net 或共享环 backend、IRQ 通知、IP 配置、TCP/UDP smoke test 和错误恢复。

- [x] manager 独占 QEMU virtio-net-pci 的 PCI I/O 与 DMA。
- [x] child 只映射独立 TX/RX 两页 bounded packet window。
- [x] 接入 LKL virtio-net 并配置 `10.0.2.15/24` 与 gateway。
- [x] 通过独立 host Ethernet peer 完成 ARP、ICMP 与 TCP 双向 payload 验证。
- [x] 保留 100 轮 ext4/BusyBox 回归并验证网络 mapping/cap 回收。

验收门槛：LKL 能完成 ICMP、TCP client/server 和持续数据校验，timer 超时行为纳入测试。

完成状态：Phase 2.5 已完成。当前 QEMU 缺少 slirp，`run.sh` 使用 QEMU socket netdev 与
`tools/net-peer.py` 构成外部确定性二层链路；所有包仍穿过真实 virtio PCI、manager DMA、受控
Endpoint 和 child LKL 网络栈。实现与证据见 `PHASE2.5-RESULTS.md`。

### Phase 2.5.1：异步 RX 与队列压力

- [x] manager 独立 poll thread 成为 ethdriver 的唯一 `raw_poll()` owner。
- [x] child 只获得 receive-only RX Notification cap。
- [x] 无包时 LKL poll thread 阻塞，不再产生空 Endpoint polling。
- [x] 32-entry bounded queue 实现 high-water、backpressure、drop、empty-fetch 统计。
- [x] 通过 64×1200-byte UDP burst 验证慢消费者、overflow accounting 和恢复。

完成状态：Phase 2.5.1 已完成。实现与证据见 `PHASE2.5.1-RESULTS.md`。

### Phase 2.5.2：真实 virtio-net IRQ

- [x] 从 PCI config 动态读取 interrupt line/pin。
- [x] manager 独占 level-triggered、active-low IOAPIC capability。
- [x] 独立 IRQ thread 调用 `raw_handleIRQ()` 并在设备 ISR 清除后 ack seL4 handler。
- [x] IRQ、kick 和 fallback 路径通过 manager-private driver lock 串行化。
- [x] IRQ 初始化或运行时 wait/ack 失败时切换 polling fallback。
- [x] 自动回归要求真实 interrupt 非零且 continuous fallback poll 为 0。

完成状态：Phase 2.5.2 已完成。实现与证据见 `PHASE2.5.2-RESULTS.md`。

### Phase 2.5.3：TX bounded queue 与持续吞吐

- [x] manager 私有 16-entry TX SPSC queue 隔离 child window 与 DMA 生命周期。
- [x] queue full 返回可重试背压，child 保持数据直到 enqueue 成功。
- [x] high-water、backpressure、driver retry 和 completed 统计。
- [x] 2048×1200-byte UDP 持续发送与对端逐包完整性确认。
- [x] 一次性 TX stress gate 确定性触发 queue-full retry，UDP 丢包使用有界重传。

完成状态：Phase 2.5.3 已完成。实现与证据见 `PHASE2.5.3-RESULTS.md`。

### Phase 2.5.4：可选宿主网络后端

- [x] 保留 socket peer 为默认确定性回归。
- [x] 支持 QEMU libslirp user netdev，并复用 host TCP/UDP 测试服务。
- [x] 支持 QEMU passt netdev/helper 与固定 `10.0.2.0/24` 参数。
- [x] 支持管理员预配置的 TAP 接口并检查 `10.0.2.2/24`。
- [x] 对缺少 backend、helper 或接口给出明确错误。

完成状态：Phase 2.5.4 已完成。当前机器只具备 socket backend 的完整运行条件；其余后端完成参数、
依赖守卫和 host service 验证，实际外网能力取决于宿主环境。

## Phase 2.6：性能与资源测量

- [x] 1MiB 静态 pipeline 吞吐和延迟。
- [x] RX burst 与 2048×1200-byte TX 吞吐。
- [x] ext4/virtio-block 1MiB 顺序读写和 256 次随机读写。
- [x] 100 轮 child create/start/destroy 平均值与最大值。
- [x] BusyBox heap/worker 以及 managed frame/TCB/Notification 峰值。
- [x] `tools/benchmark.py` 汇总为机器可读 JSON。

完成状态：Phase 2.6 已完成。所有 marker 同时属于正式 smoke 验收条件。

## Phase 3：UINTR 实验

UINTR 应建立在隔离、资源生命周期、timer 和设备数据通路稳定之后。首先做独立的能力探测和
最小 ping-pong benchmark，再决定它用于 task 通知、virtqueue kick 还是其他高频路径。

验收需要同时给出：不可用 CPU 的降级路径、与 seL4 调度/安全边界的关系、吞吐和尾延迟对比，
以及不使用 UINTR 时仍能通过的功能测试。

## 推荐执行顺序

```text
独立 task 隔离
  → 可回收资源与压力测试
  → virtio-block / 持久 rootfs
  → BusyBox 用户程序
  → 网络
  → UINTR
```

除 Phase 3 UINTR 外，当前规划中的功能阶段均已完成。后续可在具备相应宿主依赖的机器上增加
slirp/passt/TAP 外网实测，或继续扩展静态 nofork applet。继续保留 `tools/smoke-test.sh`
的 100 轮生命周期、ext4 重挂载、BusyBox spawn/join 和 ICMP/TCP 作为所有结构调整的回归门槛。
