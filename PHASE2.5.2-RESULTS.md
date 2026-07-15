# Phase 2.5.2 — virtio-net INTx/IOAPIC IRQ 与 polling fallback

Phase 2.5.2 将 manager 的持续 `raw_poll()` 改为真实 QEMU virtio-net PCI interrupt delivery。manager
从设备 `00:05.0` 的 PCI config 动态读取 interrupt line/pin，在 seL4 注册 level-triggered、active-low
IOAPIC handler，并把 handler 绑定到 manager-private Notification。

独立 `luna-net-irq` thread 等待硬件 Notification。callback 先调用 ethdriver `raw_handleIRQ()`；driver
读取 virtio ISR、完成 RX/TX descriptor 和补充 RX buffer 后，再 ack seL4 IRQHandler。child 不获得
IRQ、PCI、DMA 或可发送 Notification capability，现有 receive-only RX Notification ABI 保持不变。

IRQ callback、TX kick 和 polling fallback 共享一个 manager-private driver lock，避免并发操作 virtqueue。
正常 IRQ 模式只在 child 激活或 TX enqueue kick 时做一次辅助 poll，不再在网络空闲时持续循环。

若 PCI IRQ 不可用、IOAPIC 注册失败，初始化直接使用 polling；若 IRQ wait 或 acknowledge 在运行时失败，
IRQ thread 原子切换到 fallback 并唤醒 poll thread。销毁 child 时同时等待 IRQ callback、kick poll、TX DMA
和 TX software queue quiesce。

一次完整 QEMU 结果：

```text
luna: manager virtio-net IRQ ready line=10 mode=intx
LUNA_NETWORK_IRQ_OK line=10 interrupts=1616 kick_polls=2048 fallback_polls=0
LUNA_RESTART_STRESS_OK rounds=100
SMOKE TEST PASSED
```

自动检查要求 interrupt 数量大于 0、IRQ line 在 IOAPIC 范围内、fallback poll 为 0，并继续保留
RX burst、TX sustained、BusyBox、host-file ext4 和 100 轮生命周期回归。

复现：

```sh
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```
