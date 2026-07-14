# Phase 2.1 — LKL task 隔离迁移

Phase 2.1 采用渐进迁移：在 LKL 仍由 root task 承载期间，先建立并验证最终 LKL task 将使用的
独立进程边界。这样每个迁移切片都能继续运行 Phase 1.2 的完整 QEMU 回归。

## Phase 2.1a：独立 child、fault 与重建（已完成）

构建现在包含两个 ELF：

- `luna-lkl`：当前 root manager，同时暂时保留原有 LKL host。
- `luna-lkl-task`：通过 CPIO 嵌入 root image，由 manager 动态装载；已经链接 `lkl.o`，但尚未
  在 child 内调用 `lkl_init()`。

child 的调试 ELF 约 140MiB；构建会生成一个移除 debug section 的约 14MiB CPIO 副本，避免把调试
信息永久占入 manager 映像。

启动时 manager 使用 `sel4utils_configure_process_custom()` 为 child 创建独立的 CSpace、VSpace、
TCB 和 fault endpoint，只额外 mint 一个带 badge、仅有发送权的事件 endpoint capability，以及一个
只有接收权的命令 Endpoint。manager 通过 IPC 下发资源描述和 START，child 通过事件 Endpoint 报告
`READY / LKL_LINKED / RESOURCE_CONFIGURED / DONE`。

隔离测试流程：

```text
manager 使用自身 ELF 中页对齐的私有 BSS 页
  → 创建 fault-mode child
  → child 发送 READY / LKL_LINKED，等待命令
  → manager 下发两个 resource slot 描述，再发送 START
  → child 尝试读取同一虚拟地址
  → manager 收到 VMFault，核对 fault address
  → 核对 manager 私有页内容未改变
  → 销毁 fault child
  → 创建 clean-mode child
  → 收到 READY / LKL_LINKED，manager 下发资源并发送 START，收到 DONE
  → 销毁 child
  → 继续原有 LKL boot 与 shell smoke
```

QEMU 实测输出：

```text
luna: isolation child created with private CSpace/VSpace
LUNA_LKL_CHILD_LINKED
LUNA_RESOURCE_POOL_OK
LUNA_LKL_CHILD_INIT_OK
Pagefault from [luna-lkl-task]: read fault ... vaddr: <manager-private-address>
LUNA_ISOLATION_FAULT_OK addr=<manager-private-address>
LUNA_ISOLATION_CHANNEL_OK
LUNA_ISOLATION_RESTART_OK
LUNA_ISOLATION_OK
```

`tools/qemu_smoke.py` 要求以上成功标记，并继续验证原有 LKL timer、tty、文件操作和无 fault
关机。因此这不是单独的演示程序，而是后续每个 LKL 迁移切片的回归前置条件。

## capability 边界

当前 child 持有 sel4utils 为自身建立的 CNode、VSpace root、TCB、fault endpoint，以及 manager
显式授予的 send-only 事件 endpoint 和 receive-only 命令 Endpoint。它没有获得 manager 的
CSpace、VSpace、Untyped、I/O port 或 LKL host 全局状态 capability。

## 已发现的资源约束

在当前 allocman/Untyped 布局下，隔离预检额外长期占用一个动态共享页，会使随后 LKL host 创建 TCB
时缺少可重用的 2KiB/4KiB Untyped。当前协议因此先使用 seL4 IPC message register 和 Endpoint，
不长期占用共享页。Phase 2.1b 必须先建立可计量的固定资源池，再增加共享控制页，避免靠扩大权限或
隐式消耗资源绕过问题。

## Phase 2.1b：固定资源契约（进行中）

manager 现在为每个 `luna-lkl-task` 预配置两个真实 host-thread slot：

- 独立 TCB，配置到 child CSpace/VSpace 和 child fault endpoint；
- 64 页（256KiB）栈，与当前 root-hosted LKL boot thread 相同；
- 独立 IPC buffer；
- join Notification；
- child 只得到该 TCB 和 Notification 的派生 cap，不得到 VKA 或 Untyped。

manager 不再把资源描述塞进 argv，而是通过 command Endpoint 逐个发送
`CONFIGURE_SLOT(index, total, tcb, stack, ipc, join)`，最后发送 `START`。该协议可以继续增加 slot，
不需要共享 manager 内存。

正式的 child host operations 已实现 `thread_create/detach/exit/join/self/equal/stack`。测试会通过
host-ops 同时创建两个 worker；每个 worker 使用各自 TLS image，设置不同 `__thread` 值，通过 join
Notification 结束。两个 slot 全部 join 后 child 才报告 `LUNA_RESOURCE_POOL_OK`。

资源销毁时必须先删除 child CSpace 中的 TCB/Notification 派生 cap，再将原对象交还 Untyped
allocator。旧顺序会使 allocator 认为范围已经释放，但 seL4 内核仍看到派生 cap，下一次 retype 会报
`Untyped Retype: Insufficient memory`。当前流程已经在 fault child 和 replacement child 两次完整
装载中验证回收。

最小 host operations 也已迁入 `luna-lkl-task`。child 在固定 worker 测试后调用：

```text
lkl_init(child_host_ops)
  → LUNA_LKL_CHILD_INIT_OK
  → lkl_cleanup()
```

当前 child host-ops 先提供 `memcpy/memset/memmove`，满足此 LKL 配置下 `lkl_init()` 的实际要求。
这证明 child 中执行的是自己的 LKL 全局状态，而不是 manager 的 LKL 副本。

## 下一迁移切片

Phase 2.1b 已建立两个固定线程 slot 和可扩展 command Endpoint 资源协议。下一步增加固定同步
Notification 池，并迁移 sem/mutex/TLS 与 bump memory host operations。随后按以下顺序迁移：

1. `lkl.o`、最小内存 host operations 和 `lkl_init()` 已迁入 child。
2. 正式 thread host operations 已使用固定池；下一步迁移 sem/mutex 和 TLS。
3. 在 child 内执行 `lkl_start_kernel()`，通过事件 endpoint 报告状态。
4. 最后迁移 timer、tty 和 shutdown；root manager 只保留资源分配、fault 诊断和生命周期管理。

在 child 能完整启动 LKL 之前，现有 root-hosted LKL 路径保留为可比较的稳定基线。
