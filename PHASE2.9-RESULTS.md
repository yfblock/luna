# Phase 2.9 — 网络热路径通知化与自适应批处理

Phase 2.9 在不引入 UINTR、零复制或 virtio 多队列的前提下，把 TX/RX、INTx 和 timer 热路径改为
Notification 驱动，并把正式网络口径扩展到 37.5 MiB TX 与 3 秒持续 RX。PCI 配置、virtqueue、DMA
buffer 和 IRQ authority 继续只存在于 root manager。

## 2.9.1 正式性能口径

- TX：单个连续 UDP stream，`32768 × 1200 = 39,321,600` bytes。
- RX：24 包 burst 连续运行至少 3 秒，使用 generation、missing bitmap 和 bounded retransmission
  验证完整性。
- 新增 TX enqueue/worker cycles、RX cycles、cycles/packet、batch paces、queue/manager wait、
  coalescing timer、full-batch bypass 和 hot-path `seL4_Yield()` 计数。
- `tools/benchmark.py`、`tools/qemu_smoke.py` 和 reviewed baseline 都把这些 marker 作为硬门槛。

2026-07-17 的 socket reviewed benchmark（QEMU x86_64、512 MiB、100-child）为：

| 项目 | Phase 2.8 reference | Phase 2.9 | 变化 |
|---|---:|---:|---:|
| 2048-packet TX | 4,009,374 B/s | 6,980,725 B/s | 1.74x |
| 37.5 MiB TX | 5,212,186 B/s 验收 floor | 6,787,011 B/s | floor 的 1.30x |
| pure RX p50 | 3,559,196 B/s | 6,920,613 B/s | 1.94x |
| sustained RX | 3,559,196 B/s reference | 5,393,873 B/s | 1.52x |
| pipeline | 49,227,133 B/s | 27,904,829 B/s | timer 架构取舍 |
| block random write/read | 2,474 / 2,567 IOPS | 1,770 / 1,826 IOPS | timer 架构取舍 |

正式 TX 平均 batch 为 11.188 包，最大 16；`hot_yields=0`。TX/RX cycles/packet 分别为
201,598/2,845，manager TX request 由逐包同步调用变为 Notification 驱动的 shared sequence 协议。

## 2.9.2 TX counting semaphore 与自适应 coalescing

child 的 64-entry TX ring 使用 63-token counting semaphore。producer 在队列满时阻塞，worker
释放 slot 后 `sem_up()`，不再使用 `seL4_Yield()` 背压。TX queue mutex 使用 LKL Notification
mutex；不能替换为 atomic pause lock，否则高优先级 worker 会对被抢占的 owner 形成确定性优先级反转。

固定 5 ms coalescing 已替换为 50/100/200 μs 自适应窗口：首个小流从 50 μs 开始，上一批达到
4 包或队列仍有 backlog 时提高到 200 μs，满 16 包立即绕过 timer 提交。正式结果：

```text
min_ns=50000 max_ns=200000
average_ns=118675
average_batch_milli=11188
max_batch=16
full_batch_bypass=2079
spurious_wakes=0
```

## 2.9.3 单一 driver owner 与无丢失 RX 唤醒

poll worker 是 `raw_tx()`、`raw_ack_irq()`、`raw_poll_budget()` 和 fallback `raw_poll()` 的唯一调用者。
固定 IRQ relay thread 只等待硬件 Notification、转发 badged Notification，并执行 seL4 IRQ handler
ack；不再调用 driver，也不再使用 driver spinlock 或每中断动态 IRQ allocation。

正式跨 QEMU 回归曾暴露 RX 只取走首个 batch 后停滞。失败快照证明 virtio used ring 和 IRQ 正常，
真正满的是 manager→child 31-entry queue。修复采用共享单调 `rx_produced/rx_consumed` 和
`rx_waiting` waitqueue 握手：child 先 armed waiting 再比较序号；producer 只在原子取走 waiting 时
Signal。持续 backlog 直接拉取下一批，空→非空仍由 Notification 唤醒，因此既不会丢 wakeup，也不会
退化成每包通知。5 次专项 3 秒 RX 和最终两轮跨 QEMU 都是 `retries=0`、`duplicates=0`、`stale=0`。

## 2.9.4 one-shot timer Notification

旧 child timer thread 的持续 TSC poll/`seL4_Yield()` 已替换为 manager-owned PIT one-shot IRQ 和
child wake Notification。manager 与 child 均报告 `polling_loops=0`。为避免 LKL 把较晚 deadline
反复同步 IPC 到 manager，`>=5 ms` 且不早于当前硬件 deadline 的 arm 延后到旧 one-shot 唤醒后再
re-arm；微秒级 TX coalescing timer 保持精确。manager generation 单调拒绝 stale ARM，CANCEL 先推进
generation；child 用 `task_timer_clear_epoch` 关闭 setter/service 清除旧 timer 的窄竞态。正式 clean
child 合并 34,294 次 arm，manager arm 为 30,208 次。

持续 TX 还使用独立的 5 ms backpressure-only one-shot：pending response 先执行两次 bounded driver
poll；仍无空间时由 timer 以专用 badge 唤醒 poll worker。若该超时唤醒后仍无空间，只返回一次
`accepted=0` retry，避免永久等待，同时不会退化为每释放少量 slot 就重试。正常正式 benchmark 的
`timeout_retries=0`；child teardown 会在 poll worker quiescent 后取消残留 network deadline。

这个改变消除了 timer hot polling，但改变了 Phase 2.8 block/pipeline 调度成本。reviewed baseline
保留 pipeline 25 MB/s、顺序/冷热 Block 门槛，并把稳定 random I/O reference 更新为约 1.69k IOPS；
没有通过降低网络门槛掩盖回归。

## 2.9.5 验证与后端

最终代码通过：

```sh
./setup-deps.sh --check-only
./tools/quality-gate.sh --analyze
./tools/benchmark.py --timeout 600 --check-baseline
./tools/smoke-test.sh --cross-qemu --timeout 600
git diff --check
```

- socket：最终 reviewed benchmark 和最终跨 QEMU 两轮均通过；两轮各 100-child，保留
  `fallback_polls=0`、持久化 marker 和宿主 FSCK。长 TX 对 TCG 调度较敏感，因此性能 floor 只由
  独立 reviewed benchmark 判定，不由功能型 cross-QEMU smoke 判定。
- slirp：系统 QEMU 8.2 完整 100-child smoke 通过，持续 RX 6.53 MB/s、TX 6.19 MB/s。
- passt：既有临时解包、非 root helper 记录为 RX 4.54 MB/s、TX 7.14 MB/s；本次环境的 PATH 中没有
  `passt` helper，因此未重跑。
- TAP：代码路径和 capability probe 保留；当前宿主没有 `luna-tap0`，且当前进程 effective
  capability 为空，无法配置 TAP；Phase 2.7/2.8 的既有 TAP 语义不变。

资源门槛保持：2 个 full 8192-page profile、100 个 36-page light profile、TCB 50、child
Notification 147、managed frame 8225、network window 17 页。timer IRQ 栈收紧为 2 页，poll
worker 栈收紧为 4 页，为跨 QEMU 的 full allocator audit 留出确定性 frame 余量。

## 下一阶段

Phase 2.10 可在这套稳定 Notification/batch 基线上引入 shared descriptor ring 与 reduced-copy：
目标是 TX 最多 4 次复制降到 2 次、RX 3 次降到 1 次，再评估多队列；不要重新引入 driver 多 owner、
atomic spinlock 或 hot-path polling。
