# Phase 2.8 — 数据通路稳态、通知化与批量化

Phase 2.8 在不引入 UINTR、不扩大 child PCI/DMA/VKA/Untyped capability、保留 100 轮正式生命周期
门槛的前提下，完成 Block 多样本诊断、allocator stress 分级、Block 等待通知化、真实 INTx budget
以及 BusyBox pipeline 批量化。

## 2.8.1 Block 多样本与双峰根因

旧 benchmark 只执行一次，并把首次 LKL/ext4 worker 调度、page cache、journal 状态和正式结果混为
一体。Phase 2.8 先预填充完整 1 MiB extent，再执行一次完整 warm-up，随后输出 7 个正式样本和
p50/p95/p99。每个样本同时记录：

- 顺序数据写与 fsync；
- hot/cold read、随机数据写与 fsync；
- queue wait、worker wait、manager IPC、copy、flush wait 和 completion wait；
- IPC 与 batch 数量。

`LUNA_BLOCK_BIMODAL_DIAG` 和跨 QEMU 分段表明，旧测试末尾的 `drop_caches` 会污染下一轮前置
状态，而剩余两档差异全部发生在 256 次 4 KiB ext4 buffered write 中；queue/worker/manager
IPC/copy 几乎相同。因此顺序样本改为 16 次 64 KiB syscall，随机样本继续保留 256 次 4 KiB I/O。
现在使用 `warmup=1 normalization=drop-before-sample`，正式结果不再由首次状态或小 syscall 调度决定。

2026-07-16 socket 单 QEMU 中，正式顺序写 p50 约 7.08 ms（148 MB/s），p95/p99 约 7.31 ms；
两个独立 QEMU 的范围为 113.9–148.9 MB/s。cold read 约 19.3–19.8 MB/s，random write/read
约 2.47–2.49k/2.57–2.59k IOPS。随机 I/O 数字低于旧的单样本
reference，原因是旧 reference 恰好采到首次 journal/cache 瞬态；新口径使用稳态多样本。

## 2.8.2 allocator stress 分级

fault 和 clean audit child 继续执行完整 8192 页/32 MiB 边界测试。100 个 stress child 改为 36 页
light profile，覆盖混合尺寸分配、碎片释放、地址复用、清零和全部释放。manager 在
`ALLOCATOR_OK` 时按 profile 校验：

- full：峰值必须精确为 8192 页；
- light：峰值必须在 1–64 页；
- 两种 profile 在事件点和 halt 后 mapped pages 都必须为 0。

正式 100 轮中，child start p50 从 Phase 2.7 的约 1.12 s 降至约 0.54 s，p99 约 0.59 s；full
profile 仍在首尾各执行一次，因此没有削弱完整 arena 和 teardown 审计。

## 2.8.3 Notification 替代忙等待

Block 的三个关键 `seL4_Yield()` 循环已替换为原有 96-slot 同步池内的 LKL semaphore：

- 8-entry local batch queue 使用 counting semaphore 等待空位；
- 每个 batch 使用独立 completion semaphore；
- async write flush 按发布批次数等待 completion token。

没有增加 child capability 上限。clean child 实测同步池 high-water 约 50–52/96；Block clean 路径
产生约 302 个 completion signal，queue high-water 仍达到 8，背压时不再忙轮询。

## 2.8.4 INTx budget/coalescing

项目补丁扩展 seL4 `libethdrivers` raw interface，使 virtio PCI 能拆分 ISR acknowledge 和 bounded
completion drain。manager legacy INTx callback 每次最多处理 16 个 TX/RX used entry，剩余工作通过
Notification 合并到 poll worker；inactive child 不再补充 RX descriptor。

正式实测：

```text
budget=16
max_packets_per_irq=16
budget_exhaustions=11
coalesced_polls=8
coalesced_packets=93
fallback_polls=0
```

因此 budget 是驱动 descriptor 层的真实限制，不是 callback 外部计数器。

## 2.8.5 pipeline 批量化

BusyBox `copyfd` 使用 64 KiB bounded stack buffer，静态 pipeline 创建时把 LKL pipe 扩展到 256 KiB。
1 MiB `cat | dd bs=65536` 的 7 样本结果：

```text
p50 throughput       ≈ 47.6 MB/s
p99                  ≈ 25.2 ms
average read size    ≈ 61.7 KiB
average write size   ≈ 63.6 KiB
pipe capacity        = 256 KiB
```

Phase 2.7 的 pipeline 约为 5.15 MB/s、平均 I/O 约 4.1 KiB。`cat | wc -c` EOF、二/三段 pipeline、末段
状态和 worker/background 回归继续通过。

两个独立 QEMU 的 pipeline 为 49.2–49.8 MB/s；child start p99 为 559–562 ms。两轮 resource
invariant 完全一致，并在每轮结束后通过宿主 `e2fsck -fn`。

## 依赖与能力边界

- `patches/sel4-ethdrivers-intx-budget.patch` 固定 raw IRQ budget 接口；
- `patches/busybox-pipeline-batch.patch` 固定 BusyBox copy buffer 行为；
- `setup-deps.sh --check-only` 验证两个补丁均已正确应用；
- child TCB/Notification、共享窗口、PCI/DMA 和 Untyped authority 均未扩大；
- root-only allocman metadata pool 从 8 MiB 调整为 12 MiB，为增长后的 child ELF 和跨 QEMU
  100-round teardown 元数据保留余量；该 pool 不映射或授予 child；
- persistent ext4、宿主 FSCK、socket/slirp/passt/TAP 语义和 `forbidden=0` 均保留。

## 验证

```sh
./setup-deps.sh --check-only
./run.sh --build-only
./tools/quality-gate.sh --analyze
./tools/qemu_smoke.py --timeout 600 --reset-disk
./tools/benchmark.py --timeout 600 --check-baseline
./tools/smoke-test.sh --cross-qemu --timeout 600
git diff --check
```

正式 smoke 硬性检查 2 个 full profile、100 个 light profile、102 个 Block semaphore 生命周期、7 个
Block 正式样本、INTx budget/coalescing、pipeline 平均 I/O 尺寸、100-child 回收和 persistent rootfs。
