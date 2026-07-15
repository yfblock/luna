# Phase 2.5.1 — 异步 RX、队列背压与网络压力回归

Phase 2.5.1 将 Phase 2.5 的无包 RX Endpoint polling 替换为 manager-driven 异步通知，并增加
bounded queue 的高水位、背压、丢包和空取包统计。普通收包路径现在是：

```text
QEMU virtio-net-pci
  → manager poll thread / DMA completion
  → 32-entry bounded RX queue
  → seL4 Notification
  → child LKL poll thread
  → NET_RX Endpoint 仅取一个已经就绪的 packet
```

## receive-only Notification capability

manager 持有 persistent RX Notification 和真实设备能力。每个 child 只获得该 Notification 的
receive-only 派生 cap；child 不能伪造 RX signal。`lkl_netdev.poll()` 阻塞在 `seL4_Wait()`，收到
通知后才发送一次 `NET_RX` 请求，因此无包时不再产生 Endpoint RPC。

teardown 时，child 的 `poll_hup()` 只发送一次受控 `NET_WAKE` 请求，由 manager signal Notification
唤醒 poll thread。manager 在销毁 child 前关闭 `child_active`，并等待正在执行的 driver poll 离开
critical section，然后清空 queue 和残留 signal。persistent Notification 本身不授予 child 写权限，
也不会随 child frame/cap 回收而失效。

## manager poll thread 与无优先级反转数据结构

manager 新增同一 CSpace/VSpace 内的 `luna-net-rx` thread，优先级与 child 数据面线程相同。它是
virtio ethdriver 的唯一 `raw_poll()` 调用者。RX queue 使用单生产者/单消费者 head/tail：

- producer：manager poll thread / RX completion；
- consumer：root manager 的 `NET_RX` Endpoint handler；
- packet 写完后 release-store head，consumer acquire-load head；
- consumer 复制到独立 RX transfer page 后 release-store tail。

TX 也不再让高优先级 root handler 自旋等待低优先级 poll thread。handler 发布一个 TX request 后
阻塞在 manager-private completion Notification；poll thread 提交 DMA，ethdriver completion 再唤醒
handler。这避免了 spinlock priority inversion，同时保持 TX/RX driver 调用单线程化。

## bounded queue、背压和统计

软件 RX ring 有 32 个 slot，可用容量为 31。manager 在 ring 满时停止继续调用 `raw_poll()`，记录
一次 backpressure transition；consumer 释放 slot 后 poll thread 恢复设备处理。若一次 virtio
completion batch 已超过剩余软件 slot，超出部分被显式丢弃并增加 drop counter。

每个 active child 独立重置以下计数：

- `high_water`：最大同时排队 packet 数；
- `backpressure`：进入满队列状态的次数；
- `drops`：completion batch 超出 queue 容量的 packet 数；
- `empty_fetches`：Notification 后 Endpoint 取包却发现 queue 为空的次数。

## 压力回归

clean child 在 ICMP/TCP smoke 后执行确定性压力场景：

1. 暂时 mask manager→child RX Notification，模拟慢消费者；
2. 向 host peer 发送 UDP burst trigger；
3. host peer 连续返回 64 个 UDP packet，每个 payload 为 1200 bytes，并携带 sequence/count；
4. manager queue 达到高水位并进入 backpressure；
5. 恢复 Notification，child 校验收到 packet 的长度、序号、总数和 1196-byte payload pattern；
6. 查询 queue stats，并要求 `empty_fetches=0`。

一次完整验收中的实际结果：

```text
LUNA_NET_QUEUE_STATS received=30 high_water=31 backpressure=1 drops=34 empty_fetches=0
```

该结果证明 bounded queue 在慢消费者下达到容量上限、报告 overflow drop，并在恢复通知后继续交付
有效大包。之后 BusyBox shell、ext4 sync 和正常 teardown 仍继续执行。

## 验收输出

```text
LUNA_NETWORK_ASYNC_RX_OK notification=receive-only
LUNA_NETWORK_ICMP_OK peer=10.0.2.2
LUNA_NETWORK_TCP_OK peer=10.0.2.2:18080
LUNA_NET_QUEUE_STATS received=... high_water=31 backpressure=1 drops=... empty_fetches=0
LUNA_NETWORK_PRESSURE_OK burst=64 payload=1200
LUNA_NETWORK_RECLAIM_OK rounds=100
LUNA_RESTART_STRESS_OK rounds=100
LUNA_PERSISTENCE_OK rounds=100
SMOKE TEST PASSED
```

复现：

```sh
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```

## 后续

Phase 2.5.3 已增加 TX bounded queue、可重试背压和 2048×1200-byte 持续发送完整性回归。下一步可将
manager 的物理设备 polling 替换为真正的 IRQ delivery，同时保留当前 Notification ABI；也可以增加
多队列和更长时间的吞吐/尾延迟 benchmark。
