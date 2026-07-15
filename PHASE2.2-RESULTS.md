# Phase 2.2 — 可回收资源、同步/TLS 与 thread/timer 生命周期语义

本阶段完整收尾了 Phase 2.2：删除 child ELF 中的 32MiB 静态 bump heap，改为由 child 选择虚拟页、
manager 持有分配权限并执行实际 map/unmap；补齐可计数 semaphore、mutex owner 检查、TLS destructor
多轮重入、thread join/slot reuse 和 timer cancel/rearm 并发语义；随后连续创建、启动、关闭和销毁
100 个完整 LKL child，确认 capability、页映射、线程槽和 timer 状态可以持续复用。

所有 host 语义自测都在每个 fault/stress/clean child 的 `lkl_init()` 前执行。

## 安全边界

child 仍不持有 VKA、Untyped、manager CSpace 或 manager VSpace。manager 在 child VSpace 中只保留
一段 `[0x20000000, 0x22000000)` 的 32MiB 虚拟地址范围，不预先映射物理页。

内存请求复用已有的双向控制通道：

```text
child host op
  → MEMORY_MAP / MEMORY_UNMAP(address, pages)
  → manager 校验 badge、地址范围、页对齐和当前映射位图
  → vspace_new_pages_at_vaddr() / vspace_unmap_pages(..., vka)
  → MEMORY_RESULT(status)
```

因此 allocator 可以归还真实 frame、mapping cap 和 Untyped backing，而不需要把分配 authority 交给
child。所有请求串行经过 child allocator 自旋锁，避免多个 LKL host thread 交叉接收响应。

## child allocator

`lkl_task_host.c` 维护：

- 8192 页的虚拟 arena 位图；
- 1024 个 allocation record，记录地址、页数和请求字节数；
- active allocation、active bytes 和 peak bytes 统计；
- 对 `mem_alloc/free`、`page_alloc/free`、`mmap/munmap`、`shmem_mmap` 的统一页级实现；
- 16 字节和 4096 字节对齐、释放地址校验、重复释放检测以及重新映射后的清零。

小分配当前按整页计费，这是后续可优化的内部碎片问题，但释放会立即 unmap，已不存在只增长不回收
的 bump offset。

## allocator 自测与释放断言

每个 child 在 `lkl_init()` 前运行同一套自测：

1. 混合小块与两页 allocation，释放中间块后验证同地址复用；
2. 写入 `0xa5` 后释放并重新映射，验证首尾字节恢复为 0；
3. 覆盖 `mmap/munmap` 与 `shmem_mmap/munmap`；
4. 映射并释放整个 32MiB arena，验证 8192 页容量和完整回收；
5. manager 收到 `ALLOCATOR_OK` 时要求 `mapped_pages == 0` 且 `peak_pages == 8192`。

LKL 以 `mem=16M` 启动时，`page_alloc` 会按需映射 4096 页。`lkl_sys_halt()` 的 `page_free` 完成后，
child 和 manager 分别检查本地 allocation/位图与 manager mapping 位图均归零，之后才发送
`LUNA_CHILD_ALLOCATOR_RELEASE_OK`。

移除静态 heap 后，`luna-lkl-task` 的 BSS 从约 35MiB 降到约 1.5MiB；32MiB 只是一段虚拟上限，
不再随 ELF 装载常驻分配。

## 可计数 semaphore

semaphore 不再直接把 Notification 当二值 token。每个 `lkl_sem` 现在保存原子 `count` 和 `waiters`：

- `sem_down` 先用 CAS 消费 token；无 token 时登记 waiter，再阻塞于 Notification；
- `sem_up` 原子增加 token，存在 waiter 时 signal；
- waiter 获得 token 后，如果仍有剩余 token 和 waiter，会继续 signal 传递唤醒；
- Notification 中可能存在的合并或陈旧 signal 只会导致重新检查 count，不会凭空产生 token；
- 释放仍有 waiter 的 semaphore、计数溢出都会报告错误并使 smoke 失败。

自测先以初值 3 连续执行三次 down，再累计两个 token 并消费；随后启动 4 个 worker，确认四者都已
登记为阻塞 waiter 后才执行四次 up，最终要求 4 个 worker 全部返回、`count == 0` 且 `waiters == 0`。

## mutex owner 与递归深度

mutex 继续用一个 Notification token 串行化进入，但现在显式检查 `owner` 和 `count`：

- 非 owner 或已解锁 mutex 的 unlock 被拒绝，锁状态保持不变并记录 owner error；
- 释放仍被持有的 mutex 会报告错误；
- recursive mutex 仅在相同 owner 重入时增加 depth，最后一次 unlock 才释放 Notification token。

自测由 root 持锁、worker 尝试非法 unlock，验证请求被拒绝且 owner/depth 未改变；随后 4 个 worker
在锁内主动 yield 并累计 128 次更新，验证互斥；最后验证 recursive depth `2 → 1 → 0`。

测试结束后 owner error 计数清零。LKL 正式运行期间如再出现 owner error，child 不会报告 halt 成功。

## TLS destructor 重入

线程退出时，TLS cleanup 采用与 POSIX `PTHREAD_DESTRUCTOR_ITERATIONS` 一致的最多 4 轮扫描。每轮先
清除当前 value，再调用 destructor，因此 destructor 可以通过 `tls_set()` 为同一 key 重新设置值并在
下一轮再次执行。第 4 轮后仍存在的 value 会被清除，避免固定 TLS 槽复用时泄漏到下一线程。

`tls_free` 现在与 `pthread_key_delete` 一致，只删除 key 和关联值，不主动调用 destructor。

自测包括：

- destructor 两次重新设置 key，验证总计调用 3 次；
- destructor 持续重新设置 key，验证严格停在 4 次且最终 value 为 NULL；
- key free 不额外触发 destructor。

## thread join 与 slot reuse

LKL worker 的公开 tid 不再由固定 slot 编号直接派生，而是使用单调递增的唯一值；TLS 则继续使用与
固定资源槽绑定的内部 `tls_index`。这样 slot 回收后，新线程不会继承旧 tid，也不会让旧 join 请求
误命中新线程。

thread table 自旋锁将 tid lookup、join claim、slot 释放和重新分配放在同一临界区，避免 lookup 与
复用之间的窄 ABA 窗口。每个 slot 同时记录 `exited` 和 `join_claimed`：

- 只有一个并发 joiner 能 claim 目标线程，其他 joiner 立即返回 `-1`；
- 线程正常返回或显式调用 `thread_exit()` 都先清理 TLS，再设置 exited、signal join Notification；
- join 在释放 slot 前 drain Notification，复用 slot 时也再次 drain，陈旧 signal 不会完成新 join；
- 重复 join、无效 tid 和已经复用前的旧 tid 都返回 `-1`；
- console stop 等待 console thread 正常走完退出路径，不再从外部立即 suspend。

自测覆盖两个 joiner 竞争同一目标、显式 `thread_exit()`、重复/无效/旧 tid join，以及连续 64 轮创建、
退出、join 和 slot 复用。

## timer cancel/rearm

oneshot timer 现在使用 generation 标识每次 arm。timer service 只在 armed generation 仍等于当前
generation 且 timer 仍 allocated 时执行 callback，因此旧 deadline 即使已经到期，也不能覆盖更新后的
rearm。

`timer_free()` 会在 timer operation lock 内使当前 generation 失效并清除 armed 状态；随后等待已经
开始的 callback 退出。由此保证 free 返回后，旧 callback 不会继续执行。callback 内 rearm 与其他线程
并发 free 也由同一个 operation lock 排序：先发生的 rearm 会被后续 free 取消，先发生的 free 会让
callback 内 rearm 返回失败。

自测覆盖：arm 后立即 cancel、用连续 32 次 8ms rearm 覆盖旧 1ms deadline，以及 16 轮 callback/free
竞争并断言 free 返回后的 callback 计数保持稳定。timer service TCB 在自测后继续供正式 LKL 使用。

## 100 轮完整重启压力

隔离 fault child 完成后，manager 连续运行 100 个 stress-mode child。每轮都包括：

- 独立 CSpace/VSpace、fault endpoint 和初始 TCB；
- 65 个固定 TCB（63 worker + timer + console）及其栈/IPC buffer/join Notification；
- 128 个同步 Notification 和精确 `0x3f8–0x3ff` COM1 capability；
- allocator 全 arena map/unmap 自测；
- counting semaphore、mutex owner/递归和 TLS destructor 重入自测；
- 唯一 tid、并发 join claim、显式 exit 和 64 轮 slot reuse 自测；
- generation timer 的 cancel barrier、连续 rearm 和 callback/free 竞争自测；
- `lkl_init()`、`lkl_start_kernel("mem=16M loglevel=4")`、timer 与 `lkl_sys_halt()`；
- child CSpace 派生 cap 删除、页映射释放和整个进程销毁。

`destroy_child()` 现在会检查每个 child 派生 cap 的删除结果；smoke 同时禁止 heap map/unmap 错误、
Untyped retype 错误、reservation 清理错误和残留映射。100 轮结束后再创建 clean child，原有 tty shell、
文件系统、100ms sleep、`free` 与干净关机测试继续通过。

## 验证

```sh
./run.sh --build-only
./tools/smoke-test.sh --timeout 300
```

关键输出：

```text
LUNA_CHILD_ALLOCATOR_OK pages=8192
LUNA_CHILD_ALLOCATOR_RELEASE_OK
LUNA_SYNC_TLS_OK
LUNA_THREAD_TIMER_OK
luna: restart stress 100/100
LUNA_RESTART_STRESS_OK rounds=100
LUNA_LKL_CHILD_SHELL_READY
LUNA_ISOLATION_RESTART_OK
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

本阶段实测环境为 QEMU x86_64、512MiB。最终版本连续 100 轮中未出现 capability 删除失败、页映射
残留、Untyped retype 失败、线程槽耗尽、陈旧 join/timer callback 或 allocator 错误。

## 后续

Phase 2.2 已完整完成。下一阶段进入 Phase 2.3，建立 virtio-block 数据通路和可跨重启验证的持久根
文件系统。
