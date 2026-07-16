# 数据通路与生命周期性能优化结果

本轮在不引入 UINTR、不扩大 child PCI/DMA/VKA/Untyped capability、保留 100 轮正式门槛的
前提下，完成 benchmark、网络、Block、stdio/pipeline 和 child 资源生命周期五项优化。

## 实现

- benchmark：增加 7 轮纯 RX 和 pipeline 样本、p50/p95/p99、冷热 Block 读，以及
  syscall/IPC/batch/kick/copy/queue high-water 计数。
- 网络：共享窗口扩展为 16 个 TX 和 16 个 RX slot；child 使用 64-entry bounded TX queue
  和异步 drain worker；manager 支持批量 Endpoint、16 个 outstanding TX DMA descriptor，
  并只在 empty-to-non-empty 时 kick。
- Block：64 KiB 窗口划分为 header 和 15 个 4 KiB slot；child 使用 8-entry bounded
  write-back batch queue，manager 批量完成；read 和 flush 会先等待所有异步写，flush descriptor
  形成严格 barrier。同步 read 保留 direct shared-window 路径，避免 worker 调度延迟。
- stdio：`fread`/`fwrite` 使用最大 64 KiB 块操作，标准输入逐字符接口使用每 worker 16 KiB
  缓冲；恢复 `cat file | wc -c` EOF 回归。
- 生命周期：通用 TCB pool 从 63 缩到 47，sync Notification 从 128 缩到 96；实现 detached
  thread 的外部 suspend/reap 和安全 slot 复用，保留唯一 tid、TLS cleanup、join claim 和 ABA 检查。

PCI I/O、virtio DMA、persistent backing 和所有 VKA/Untyped authority 仍只存在于 root manager。

## 最终 benchmark

2026-07-16，QEMU x86_64、512 MiB、socket backend，正式 100 轮：

| 项目 | Phase 2.6 基线 | 当前结果 | 变化 |
|---|---:|---:|---:|
| pipeline 1 MiB p50 | 5,024,665 B/s | 5,151,620 B/s | 1.03x |
| network TX 2048x1200 | 3,396,449 B/s | 8,556,420 B/s | 2.52x |
| block sequential write | 15,093,075 B/s | 40,001,181 B/s | 2.65x |
| block random write | 6,642 IOPS | 13,004 IOPS | 1.96x |
| block random read | 2,647 IOPS | 15,733 IOPS | 5.94x |
| child create average | 556,833,721 ns | 511,406,680 ns | -8.2% |
| child start average | 1,154,472,915 ns | 1,117,573,301 ns | -3.2% |
| child destroy average | 328,868,335 ns | 277,363,146 ns | -15.7% |
| child TCB / Notification | 66 / 194 | 50 / 146 | -24.2% / -24.7% |

新增口径：

| 项目 | 结果 |
|---|---:|
| pure RX p50 / p95 / p99 | 8,077,260 / 16,036,317 / 16,036,317 ns |
| pure RX p50 throughput | 3,565,565 B/s |
| block cold / hot read | 86,258,016 / 114,696,871 B/s |
| pipeline p50 / p95 / p99 | 203,542,939 / 210,701,046 / 210,701,046 ns |
| lifecycle create p50 / p95 / p99 | 510,701,043 / 520,159,398 / 522,485,881 ns |
| lifecycle destroy p50 / p95 / p99 | 277,477,939 / 281,931,710 / 283,192,420 ns |

网络 manager 在统计点处理 2,065 个 TX packet，仅发生 154 次 TX Endpoint 请求和 59 次
empty-to-non-empty kick。RX 处理 204 个 packet，发生 21 次 batch Endpoint 请求。clean child 的
Block 路径记录 606 个 request、87 次 manager IPC、40 个 write/flush batch；bounded queue
high-water 为 8，manager descriptor batch high-water 为 15。

RX pressure 的 339 KB/s 仍包含故意设置的 100 ms consumer pause，只用于背压回归；纯 RX
吞吐使用独立的无 pause 7 轮 benchmark。Block hot/cold 在旧热读/随机 I/O 完成后通过
`drop_caches`/`fadvise` 单独测量，不再改变旧指标的前置状态。

## 验收

```sh
./setup-deps.sh --check-only
./run.sh --build-only
./tools/qemu_smoke.py --timeout 600 --reset-disk
./tools/smoke-test.sh --cross-qemu --timeout 600
./tools/benchmark.py --timeout 600
git diff --check
```

最终结果：单 QEMU 100 轮通过；两个独立 QEMU 各 100 轮通过；跨 QEMU 文件持久化通过；
宿主 `e2fsck` 通过；网络 RX/TX 压力、BusyBox/pipeline、`forbidden=0` 和资源回收均通过。

## Phase 2.8 稳态口径

上面的 Block 数字来自旧的一次性样本，容易采到首次 cache/journal/worker 瞬态。Phase 2.8 已用一次
完整 warm-up 加 7 个正式样本取代该口径；详细结果见 `PHASE2.8-RESULTS.md`。当前 reviewed
socket 稳定性 reference 为：pipeline 约 49.2 MB/s，Block 64KiB 顺序写约 113.9 MB/s，随机写/读约
2.47k/2.57k IOPS，child start p50 约 0.54 s。随机 IOPS reference 的下降是统计口径修正，不是把
首次快样本继续当成稳态吞吐。
