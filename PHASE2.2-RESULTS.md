# Phase 2.2 — 可回收 child 页映射与生命周期压力

本阶段完成 Phase 2.2 的内存与重复生命周期部分：删除 child ELF 中的 32MiB 静态 bump heap，改为
由 child 选择虚拟页、manager 持有分配权限并执行实际 map/unmap；随后连续创建、启动、关闭和销毁
100 个完整 LKL child，确认固定 capability、页映射和线程资源可以持续复用。

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

## 100 轮完整重启压力

隔离 fault child 完成后，manager 连续运行 100 个 stress-mode child。每轮都包括：

- 独立 CSpace/VSpace、fault endpoint 和初始 TCB；
- 65 个固定 TCB（63 worker + timer + console）及其栈/IPC buffer/join Notification；
- 128 个同步 Notification 和精确 `0x3f8–0x3ff` COM1 capability；
- allocator 全 arena map/unmap 自测；
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
luna: restart stress 100/100
LUNA_RESTART_STRESS_OK rounds=100
LUNA_LKL_CHILD_SHELL_READY
LUNA_ISOLATION_RESTART_OK
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

本阶段实测环境为 QEMU x86_64、512MiB。连续 100 轮中未出现 capability 删除失败、页映射残留、
Untyped retype 失败、线程槽耗尽或 allocator 错误。

## 后续

Phase 2.2 剩余项是可计数 semaphore、mutex owner 错误处理、TLS destructor 重入语义，以及更细的
thread join 与 timer cancel/rearm 并发压力测试。
