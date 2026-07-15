# Phase 2.5.3 — TX bounded queue、背压与持续吞吐

Phase 2.5.3 将 child 的单页 TX window 与 manager 私有 virtio DMA 生命周期解耦。manager 在收到
`NET_TX` 请求后先复制 Ethernet frame 到 16-entry SPSC queue，再立即允许 child 复用共享页；独立
kick/IRQ thread 逐项提交到 virtio-net-pci，TX completion 主要由 INTx handler 回收。

队列实际容量为 15。queue full 时 manager 返回 `LUNA_NET_TX_RETRY`，child 保持共享页内容并在 yield
后重试，直到成功 enqueue；该路径不会将暂时背压变成 LKL virtqueue I/O error，也不会扩大 child 的
PCI、DMA 或 Notification capability。

统计覆盖：

- queue high-water；
- queue-full backpressure retry；
- ethdriver `raw_tx()` 暂时失败重试；
- 已完成的硬件 TX packet。

自动压力测试发送 2048 个 1200-byte UDP packet。每包包含 sequence、总包数和确定性 payload，宿主
peer 仅在收到并验证全部 sequence 后返回 ACK。一次完整结果为：

```text
LUNA_NET_TX_QUEUE_STATS sent=2048 high_water=15 backpressure=335 driver_retries=0 completed=2057 elapsed_ns=660554554
LUNA_NETWORK_TX_PRESSURE_OK packets=2048 payload=1200
```

完整 smoke 继续通过 100 轮 child boot/halt/destroy、RX burst、ext4、BusyBox、ICMP 和 TCP 回归。

复现：

```sh
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```
