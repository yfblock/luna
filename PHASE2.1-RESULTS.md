# Phase 2.1 — LKL task 隔离迁移

Phase 2.1 已完成：LKL 只在独立的 `luna-lkl-task` 中链接和运行，root `luna-lkl` 只负责资源分配、
PIT/TSC 校准、fault 诊断和 child 生命周期。以下各节保留渐进迁移过程，并以 Phase 2.1d 的最终
结构为当前状态。

## Phase 2.1a：独立 child、fault 与重建（已完成）

构建现在包含两个 ELF：

- `luna-lkl`：root manager；在 2.1a 迁移阶段曾暂时保留原有 LKL host。
- `luna-lkl-task`：通过 CPIO 嵌入 root image，由 manager 动态装载，并独立链接、启动和关闭
  自己的 `lkl.o` 实例。

child 的调试 ELF 约 140MiB；构建会生成一个移除 debug section 的约 14MiB CPIO 副本，避免把调试
信息永久占入 manager 映像。

启动时 manager 使用 `sel4utils_configure_process_custom()` 为 child 创建独立的 CSpace、VSpace、
TCB 和 fault endpoint，只额外 mint 一个带 badge、仅有发送权的事件 endpoint capability，以及一个
只有接收权的命令 Endpoint。manager 通过 IPC 下发资源描述和 START，child 通过事件 Endpoint 报告
`READY / LKL_LINKED / RESOURCE_CONFIGURED / LKL_INIT_OK / LKL_BOOT_OK / LKL_HALT_OK / DONE`。

隔离测试流程：

```text
迁移阶段先运行原 root LKL tty/shell 回归并正常 halt/cleanup
  → manager 使用当时仅 root ELF 映射的页对齐 probe
  → 创建 fault-mode child
  → child 发送 READY / LKL_LINKED，等待命令
  → manager 下发 64 个 thread slot、128 个 sync slot 和 TSC frequency
  → child lkl_init → kernel boot → timer → halt
  → child 尝试读取 manager probe 的同一虚拟地址
  → manager 收到 VMFault，核对 fault address
  → 核对 manager 私有页内容未改变
  → 销毁 fault child
  → 创建 clean-mode child
  → 再次完成 lkl_init → kernel boot → timer → halt，收到 DONE
  → 销毁 child
  → LUNA_SHUTDOWN_OK
```

QEMU 实测输出：

```text
luna: isolation child created with private CSpace/VSpace
LUNA_LKL_CHILD_LINKED
LUNA_RESOURCE_POOL_OK
LUNA_LKL_CHILD_INIT_OK
LUNA_LKL_CHILD_BOOT_OK
LUNA_LKL_CHILD_HALT_OK
Pagefault from [luna-lkl-task]: read fault ... vaddr: <manager-private-address>
LUNA_ISOLATION_FAULT_OK addr=<manager-private-address>
LUNA_ISOLATION_CHANNEL_OK
LUNA_ISOLATION_RESTART_OK
LUNA_ISOLATION_OK
```

`tools/qemu_smoke.py` 要求以上成功标记，并继续验证 LKL timer、tty、文件操作和无 fault
关机。因此这不是单独的演示程序，而是后续每个 LKL 迁移切片的回归前置条件。

## capability 边界

当前 child 持有 sel4utils 为自身建立的 CNode、VSpace root、TCB、fault endpoint，以及 manager
显式授予的 send-only 事件 endpoint 和 receive-only 命令 Endpoint。它没有获得 manager 的
CSpace、VSpace、Untyped、I/O port 或 LKL host 全局状态 capability。

## 已发现的资源约束

在当前 allocman/Untyped 布局下，隔离预检额外长期占用一个动态共享页，会使随后 LKL host 创建 TCB
时缺少可重用的 2KiB/4KiB Untyped。当前协议因此先使用 seL4 IPC message register 和 Endpoint，
不长期占用共享页。Phase 2.1b 已通过可计量的固定资源池解决这一约束，没有靠扩大 child 权限或
隐式消耗资源绕过问题。

## Phase 2.1b：固定资源契约与 child kernel boot（已完成）

manager 现在为每个 `luna-lkl-task` 预配置 65 个真实 host-thread slot：

- 独立 TCB，配置到 child CSpace/VSpace 和 child fault endpoint；
- 64 页（256KiB）栈；
- 独立 IPC buffer；
- join Notification；
- child 只得到该 TCB 和 Notification 的派生 cap，不得到 VKA 或 Untyped。

其中 63 个用于 LKL host thread，1 个专供 polling oneshot timer，1 个专供 COM1 console poll。
manager 还预建 128 个同步 Notification，供 sem/mutex 使用。child 不持有 VKA、Untyped 或
manager CSpace。

manager 不再把资源描述塞进 argv，而是通过 command Endpoint 逐个发送
`CONFIGURE_SLOT(index, total, tcb, stack, ipc, join)`，最后发送 `START`。该协议可以继续增加 slot，
不需要共享 manager 内存；同步 cap 通过 `CONFIGURE_SYNC` 逐个发送，`START` 同时携带 manager
使用 PIT 校准得到的 TSC frequency 数值。

tty 迁移只授予 child `0x3f8–0x3ff` 的精确 I/O-port capability，而不是 manager 的 I/O ops、VKA
或整个 CSpace。child console TCB 轮询 line-status/data port，把字符写入私有 SPSC ring，并调用
`lkl_trigger_irq()`；LKL `console_take` 在 IRQ 上下文排空 ring，现有 n_tty 继续负责回显和行编辑。

正式的 child host operations 已实现 `thread_create/detach/exit/join/self/equal/stack`。资源测试会
同时启动 63 个 worker；每个 worker 使用各自 TLS image，设置不同 `__thread` 值，通过 join
Notification 结束。全部 join 后 child 才报告 `LUNA_RESOURCE_POOL_OK`。

资源销毁时必须先删除 child CSpace 中的 TCB/Notification 派生 cap，再将原对象交还 Untyped
allocator。旧顺序会使 allocator 认为范围已经释放，但 seL4 内核仍看到派生 cap，下一次 retype 会报
`Untyped Retype: Insufficient memory`。当前流程已经在 fault child 和 replacement child 两次完整
装载中验证回收。

完整 kernel boot 所需的 host operations 已迁入 `luna-lkl-task`：

- 128-slot Notification sem/mutex pool；
- 16 个 TLS key，按固定 tid 保存数据并执行 destructor；
- 32MiB 页对齐 bump heap，支持 mem/page/mmap/shmem；
- setjmp/longjmp、memcpy/memset/memmove、print/panic；
- manager 校准频率 + child TSC 纳秒换算；
- 专用固定 TCB polling oneshot timer，通过 `lkl_trigger_irq()` 注入 timer IRQ。

child 在固定 worker 测试后调用：

```text
lkl_init(child_host_ops)
  → LUNA_LKL_CHILD_INIT_OK
  → lkl_start_kernel("mem=16M loglevel=4")
  → LUNA_LKL_CHILD_BOOT_OK
  → lkl_sys_halt()
  → LUNA_LKL_CHILD_HALT_OK
```

隔离 child 在 halt 后不执行进程内 `lkl_cleanup()`：manager 会 suspend 所有固定 TCB，删除 child
CSpace 中的派生 cap，再销毁整个 CSpace/VSpace。LKL/KASAN 全局状态随进程一起消失，这比在即将
销毁的 task 内复位全局状态更符合最终生命周期边界。

child 使用 `mem=8M` 时会在完整文件系统初始化阶段 OOM，因此使用 `mem=16M` 和 32MiB host heap。
child ELF 扩大后，旧 manager probe 地址一度落入 child BSS；2.1d 已用 manager VSpace 中
`0x40000000` 的动态私有映射取代对 ELF 尾部布局的依赖。

加载几十 MiB child ELF 需要为数千个 frame/mapping 保存 allocman metadata。root 静态 metadata
pool 从 1MiB 扩到 4MiB；移除 root LKL 后完整 smoke 已在 512MiB QEMU 内存下通过，`run.sh` 也已
改用该默认值，可用 `LUNA_QEMU_MEM` 覆盖。

## Phase 2.1c：child tty/shell（已完成）

`shell.c` 不再直接依赖 root host 全局状态：时间源通过函数指针注入，设备节点、fd 0/1/2 和 procfs
由 `luna_shell_prepare()` 在调用方的 LKL 实例中建立。clean-mode child 在 boot 后报告
`LUNA_LKL_CHILD_SHELL_READY` 并进入 shell；fault-mode child 不等待输入，自动 halt 后触发隔离 fault。

自动 smoke 的长未知命令、`time`、100ms sleep、mkdir/write/cat、`free` 和 `exit` 现在全部在 child
独立 VSpace 的 LKL 中执行。

## Phase 2.1d：移除 root-hosted LKL（已完成）

最终迁移删除了 root target 中的 `lkl.o`、旧 host operations、shell 和静态 manager probe 对象。
manager 现在独立取得 I/O-port ops，使用 PIT 校准 TSC 后把频率通过控制协议交给 child；LKL 内核
对象和完整 host backend 只属于 `luna-lkl-task`。

隔离 probe 在 manager VSpace 的固定虚拟地址 `0x40000000` 动态 reserve/map。fault child 读取同一
地址时产生预期 VMFault；manager 验证私有页内容未改变，销毁 child 后 unmap 页面并释放 reservation。
链接产物检查确认 `luna-lkl` 不含 `lkl_init`、`lkl_start_kernel` 或 `lkl_sys_halt`，而
`luna-lkl-task` 仍包含这些符号。

## 当前自动验证顺序

```text
fault-mode child boot → timer → halt → manager-private VMFault
  → 销毁 child 和全部固定资源
clean-mode child boot → timer → tty shell smoke → halt → DONE
  → LUNA_ISOLATION_RESTART_OK → LUNA_SHUTDOWN_OK
```

实测关键标记：

```text
LUNA_LKL_CHILD_INIT_OK
LUNA_LKL_CHILD_BOOT_OK
LUNA_LKL_CHILD_HALT_OK
LUNA_LKL_CHILD_SHELL_READY
LUNA_ISOLATION_FAULT_OK
LUNA_ISOLATION_RESTART_OK
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

## 后续阶段

Phase 2.1 的隔离迁移已经结束。下一阶段是 Phase 2.2：用可回收页分配器替代 child 的 32MiB bump
heap，补齐可计数 semaphore 与同步语义，并加入重复启动/关机压力测试。详见 `next-plan.md`。
