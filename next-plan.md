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

- 用可回收页分配器替代 32MiB bump heap，记录映射和释放。
- 实现真正可计数的 semaphore，并补齐 mutex owner 校验。
- 扩展 TLS destructor 的多轮/重入语义测试，并处理 destructor 再次设置 key 的情况。
- 增加并发创建、退出、join、timer cancel/rearm 和重复启动/关机压力测试。

验收门槛：连续启动/关机 LKL 100 次，无 capability、页映射或线程槽持续增长。

## Phase 2.3：virtio-block 与持久根文件系统

目标：让 LKL 使用真实块设备镜像，而不只依赖内存文件系统。

主要工作：

- 建立 seL4 host 侧 virtio-mmio/block backend。
- 向 LKL 注入设备描述并完成 IRQ/共享队列连接。
- 挂载 ext2/ext4 镜像作为持久文件系统。
- 增加断电重启、一致性检查和大文件读写测试。

验收门槛：QEMU smoke test 能创建文件、正常关机、再次启动并读取同一文件，镜像通过 `e2fsck -fn`。

## Phase 2.4：真实用户程序与 BusyBox

目标：从宿主内置命令升级为运行静态用户程序。

主要工作：

- 明确 LKL 当前“宿主线程即 init”模型下的用户 ABI 边界。
- 实现静态 syscall shim、进程启动参数和 fd 继承。
- 补齐 clone/fork/exec/wait 所需的 host thread 与地址空间支持。
- 在持久根文件系统中启动 BusyBox，并逐步替换当前 C 内置 shell。

验收门槛：自动测试通过 BusyBox 执行 `sh -c 'echo ok > /tmp/x; cat /tmp/x'` 并正常回收子进程。

## Phase 2.5：网络数据通路

目标：接通 LKL 内核网络栈与 seL4/QEMU 外部网络。

主要工作：virtio-net 或共享环 backend、IRQ 通知、IP 配置、TCP/UDP smoke test 和错误恢复。

验收门槛：LKL 能完成 ICMP、TCP client/server 和持续数据校验，timer 超时行为纳入测试。

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

下一次迭代从 Phase 2.2 的可回收资源开始，并保留当前 `tools/smoke-test.sh` 作为所有结构调整的
回归门槛。Phase 2.1 完成后已验证并将默认 QEMU 内存由 1GiB 收紧到 512MiB。
