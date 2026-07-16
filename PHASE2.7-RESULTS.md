# Phase 2.7 — 稳定化、错误回滚与回归门槛

本阶段不引入 UINTR，也不扩大 child 的 PCI、DMA、VKA 或 Untyped capability。目标是把已有功能从
“一次运行通过”提升为能审计错误路径、检测资源漂移并在不同宿主网络后端上给出明确结果的工程基线。

## Child 创建回滚审计

manager 在正常 child 启动前依次注入七个中止点：

1. 独立 process/CSpace/VSpace 配置完成；
2. 32 MiB heap reservation 完成；
3. 64 KiB disk window 映射完成；
4. network batch window 映射完成；
5. TCB/Notification 固定资源池完成；
6. receive-only control cap mint 完成；
7. send-only command cap mint 完成。

每个中止点都必须完整执行 `destroy_child()`，结构清零，并让下一阶段重新分配成功，最终输出：

```text
LUNA_CHILD_START_ROLLBACK_OK stages=7
```

首次运行该审计发现 sel4utils 会把未触碰、large-page 对齐的 heap reservation 压缩为 mid-level
`RESERVED`，直接 teardown 会打印 `Cannot clear reserved entries mid level`。当前清理路径会在每个
large-page span 物化一页页表元数据再释放 reservation；正式 smoke 已确认该错误不再出现。

fault/stress child 不再初始化无用网络，只保留受控 window 映射与回收；只有 clean child 启用网络并
输出 IPv4/virtio marker。

## 性能与长期稳定性门槛

`tools/performance-baseline.json` 分成两类数据：

- `minimums`/`maximums`/`equals` 是自动失败的硬门槛；吞吐下限按多轮宿主抖动设置，用于捕获灾难性
  退化，资源和 capability 数量要求精确相等。
- `reference` 保存 reviewed 优化结果，用于报告相对百分比，不把单次宿主调度或页缓存抖动误判为
  功能失败。

运行：

```sh
./tools/benchmark.py --check-baseline
./tools/stability.py --rounds 3 --timeout 600 --log-dir artifacts/stability
```

稳定性驱动在同一个 16 MiB ext4 backing 上启动多个独立 QEMU。第一轮写入随机持久标记，后续轮次
必须读回；每轮都执行完整 100-child gate、benchmark 解析和宿主 `e2fsck -fn`，最后比较八项资源
不变量。

2026-07-16 的两 QEMU 验证结果：

| 项目 | p50 / 范围 |
|---|---:|
| pipeline | 5.10 MB/s，5.10–5.15 MB/s |
| pure RX | 3.53 MB/s，3.53–3.63 MB/s |
| TX | 9.39 MB/s，9.39–9.48 MB/s |
| block sequential write | 14.8 MB/s，14.8–16.9 MB/s |
| block cold/hot read | 19.8 / 113.8 MB/s |
| random write/read | 6,214 / 2,531 IOPS |
| child create/start/destroy p99 | 516 / 1,145 / 284 ms |

block 指标在独立 QEMU 间仍有明显双峰方差，因此 reference 对比保留可见性，但硬门槛采用保守下限。
网络、pipeline、生命周期和资源计数在连续运行中稳定。

资源不变量两轮完全一致：

```text
managed_frame_pages=8225
child_tcbs=50
child_notifications=146
child_heap_pages=8192
disk_window_pages=16
network_window_pages=17
child_thread_slots=47
child_sync_slots=96
```

## 网络后端实测

`tools/network-backends.py` 检查所选 QEMU 的 backend、passt helper 和 TAP 配置。`LUNA_QEMU` 可显式
选择另一套 QEMU，例如系统 QEMU 提供 slirp、用户安装的 QEMU 提供 passt。

| 后端 | 当前实测结果 |
|---|---|
| socket | 完整通过，ICMP/TCP/RX/TX/100 轮/FSCK |
| slirp | 使用 `/usr/bin/qemu-system-x86_64` 完整通过 |
| passt | helper 完整通过 TCP、RX/TX 压力、100 轮和 FSCK；该 helper 对 gateway ICMP 明确报告 unavailable |
| TAP | 宿主临时 TAP 完整通过；测试结束后接口已删除 |

TAP 必须配置 `10.0.2.2/24`，禁用该接口 IPv6，并关闭 multicast。否则宿主 IPv6/多播包会在当前
单核、legacy INTx 配置中形成持续中断，严重影响 child 调度。`run.sh` 和后端探测工具现在会在启动前
拒绝这种不安全配置并给出修复命令。

passt 的 ARP 和 IPv4 数据通路正常，但旧 helper 不一定响应映射 gateway 的 ICMP echo。协议新增
`LUNA_ISOLATION_EVENT_NETWORK_ICMP_UNAVAILABLE`，不会伪造成功；仅 passt 回归接受该 marker，随后
TCP echo、64-packet RX、7 轮纯 RX 和 2048-packet TX 完整性仍必须全部通过。socket、slirp、TAP
继续强制 `LUNA_NETWORK_ICMP_OK`。

## 工程门槛

`tools/quality-gate.sh` 默认执行依赖审计、所有 shell 语法检查、Python bytecode 编译、单元测试和
`git diff --check`；若宿主安装 shellcheck 也会自动运行。`--build` 增加完整构建，`--smoke` 增加跨
QEMU 持久化和 benchmark baseline。`--analyze` 使用构建生成的 compilation database，对 Luna 的
9 个 C translation unit 重新运行 GCC `-fanalyzer`；当前结果无 analyzer warning。

QEMU 驱动在观察到 forbidden marker 后会立即终止失败实例，不再等待全局超时。最终验证包括：

```text
dependency check passed
Python unit tests: 4 passed
GCC -fanalyzer: 9 files passed
full build: passed
socket smoke: passed
slirp smoke: passed
passt smoke: passed with explicit ICMP unavailable
TAP smoke: passed
two-QEMU stability: passed
host e2fsck after every formal run: passed
```
