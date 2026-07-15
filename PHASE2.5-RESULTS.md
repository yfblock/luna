# Phase 2.5 — virtio-net 网络数据通路

Phase 2.5 已接通外部主机对端到 child LKL 网络栈的完整二层路径：

```text
host UDP Ethernet peer
  ↕ QEMU socket netdev
QEMU virtio-net-pci (00:05.0)
  ↕ PCI I/O + DMA, manager only
root manager ethdriver
  ↕ validated Endpoint requests + 2 bounded pages
child LKL virtio-net
  ↕ IPv4 / ICMP / TCP
10.0.2.15
```

自动回归完成 ARP、ICMP echo 和 TCP 三次握手/双向 payload echo，并继续保留 Phase 2.4 的
100 轮 ext4/BusyBox 回归。

## capability 与传输边界

真实设备 capability 只留在 root manager：

- QEMU 固定创建 legacy `virtio-net-pci`，BDF 为 `00:05.0`；
- manager 获取 PCI config I/O port、启用 I/O space/bus mastering，并使用 page DMA；
- child 不获得 PCI config、网卡 BAR、IRQ、DMA、VKA、Untyped 或 manager CSpace/VSpace；
- child 只映射 8KiB 网络窗：4KiB TX 页和 4KiB RX 页；
- 单包上限为 2048 bytes，manager 校验 badge、event、length 和空保留字段；
- block、heap、network 请求共用 child manager-request lock，Endpoint 响应不会交叉。

manager 的 virtio PCI ethdriver 使用 64 个 manager-owned RX DMA buffer 和 32-entry bounded
software RX queue。TX 在 manager DMA buffer 中同步完成；RX 由 child LKL poll thread 请求，manager
轮询真实 virtqueue 后只复制一个完整 Ethernet frame 到 RX 页。

## LKL virtio-net 与 IPv4

child 在 `lkl_start_kernel()` 前调用 `lkl_netdev_add()` 注册自定义 host backend；启动后配置：

```text
MAC      52:54:00:12:34:56
IPv4     10.0.2.15/24
gateway  10.0.2.2
MTU      1500
```

backend 的 `tx/rx/poll/poll_hup` 只使用受限共享窗和 manager Endpoint。teardown 先让 poll thread
观察 HUP 并 join，再清理 virtio-net。ext4 block backend 保留到 `lkl_sys_halt()` 之后，确保关机阶段的
最后 superblock 写入仍有有效设备。

## 外部测试对端

当前环境的 QEMU 没有编译 `user/slirp` netdev，但提供 `socket` backend。`run.sh` 因此自动启动
`tools/net-peer.py`，并通过两个 localhost UDP port 与 QEMU 交换真实 Ethernet frame。该进程实现：

- ARP reply：`10.0.2.2` → `52:54:00:12:34:01`；
- ICMP echo reply，校验 echo id/sequence；
- TCP port 18080 的 SYN/SYN-ACK/ACK、带 checksum 的双向 payload echo。

这不是 child 内部 loopback：每个验证包都穿过 LKL virtio-net、manager Endpoint、manager DMA、
QEMU virtio PCI 设备和独立主机进程。选择 UDP Ethernet peer 也让回归不依赖宿主 TAP 权限或外部网络。

## 生命周期回归

fault 和 clean child 都创建真实 LKL virtio-net；100 个 stress child 继续完整 boot/mount/halt/destroy，
同时映射并回收两页网络窗，但不重复初始化物理网卡数据面。这样验证每轮 duplicate frame cap 和 VSpace
mapping 的回收，同时避免无意义地把 100 轮回归变成持续网络轮询基准。

关键输出：

```text
LUNA_VIRTIO_NET_OK backend=qemu-virtio-pci
LUNA_NETWORK_IPV4_OK address=10.0.2.15/24
LUNA_NETWORK_RECLAIM_OK rounds=100
LUNA_NETWORK_ICMP_OK peer=10.0.2.2
LUNA_NETWORK_TCP_OK peer=10.0.2.2:18080
LUNA_RESTART_STRESS_OK rounds=100
LUNA_PERSISTENCE_OK rounds=100
LUNA_BUSYBOX_OK command=echo ok > /tmp/x; cat /tmp/x
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

复现：

```sh
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```

## 后续

Phase 2.5 的单队列外部网络数据通路已完成。后续可以增加异步 RX Notification/IRQ，减少当前 poll
Endpoint 的调度开销；也可以在具备 slirp、passt 或 TAP 权限的环境加入 DHCP/DNS/互联网互通，或进入
Phase 3 的 UINTR 通知实验。上述扩展不能扩大 child 的 PCI/DMA capability 集合。
