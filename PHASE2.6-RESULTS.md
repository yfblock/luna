# Phase 2.6 — 扩展 applet、网络后端与性能资源测量

本阶段扩展静态 BusyBox 使用面，为宿主网络增加可选 backend，并把数据通路和资源峰值变成正式、
可重复的 QEMU 验收 marker。UINTR、native ELF `fork/exec` 和 child capability 边界均未改变。

## BusyBox 与静态 runtime

新增 applet：

```text
cmp sort grep tee dd find ls cp mv
```

ABI 新增 LKL directory stream、`access/chown/lchown/mknod/utimes`、虚拟 stdin `getchar`、无缓冲
stdio 配置和设备号转换。自动回归在 `/run` ramfs 中验证复制、比较、排序、匹配、tee、块复制、
递归 find、目录枚举和 rename。

负向回归验证：不存在命令、不支持的 pipeline、超过 4 段的 pipeline 和第 5 个并发 worker 都返回
126/127 类确定状态，不进入 native fork/exec；4 个 `sleep` 后台 worker 同时占槽后由 `wait` 回收。
正式统计为：

```text
LUNA_STATIC_RUNTIME_OK workers=30 pipelines=4 background=5
LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0
```

## 网络后端

`run.sh` 支持：

```text
socket  默认 QEMU UDP socket + 确定性二层 net-peer
slirp   QEMU user/libslirp + host L3/L4 service
passt   QEMU passt backend + passt helper + host L3/L4 service
tap     管理员预配置 TAP + host L3/L4 service
```

`tools/net-service.py` 提供 TCP echo、64-packet RX burst 和 TX stream 完整性 ACK。默认 socket 后端
仍是正式跨 QEMU 回归。当前开发机 QEMU 没有 libslirp user backend，PATH 中没有 passt helper，且
没有预配置 TAP；三种可选模式的参数生成、依赖守卫和 host service 已验证，完整外网验证需在具备
对应宿主条件的机器执行。

## Benchmark marker

正式 smoke 现在验证：

- `LUNA_PIPELINE_BENCHMARK_OK`：1MiB 文件经 `cat | dd` 的真实 LKL pipe 吞吐，并校验输出大小。
- `LUNA_NET_QUEUE_STATS ... elapsed_ns=`：RX burst 接收路径。
- `LUNA_NET_TX_QUEUE_STATS ... elapsed_ns=`：2048×1200-byte TX 路径。
- `LUNA_BLOCK_BENCHMARK_OK`：1MiB ext4 顺序读写和 256 次 4KiB 随机读写。
- `LUNA_LIFECYCLE_*_SAMPLE`：100 轮 child create/start/destroy 独立样本；汇总工具计算平均值和最大值。
- `LUNA_RESOURCE_PEAK_OK`：managed frame page、child TCB 和 Notification 数量。
- `LUNA_USER_RESOURCE_PEAK_OK`：BusyBox heap 和静态 worker 峰值。

运行：

```sh
./tools/benchmark.py
./tools/benchmark.py --net-backend passt
./tools/benchmark.py --input saved-qemu.log
```

工具在正常 smoke 通过后输出 JSON，便于保存基线和比较回归。

## 当前基线结果

2026-07-16 在 QEMU x86_64、512MiB、默认 socket backend 上完成一次独立 100 轮 benchmark：

| 项目 | 结果 |
|---|---:|
| pipeline 1MiB | 5,024,665 bytes/s |
| network RX stress | 335,859 bytes/s |
| network TX 2048×1200 | 3,396,449 bytes/s |
| block sequential write | 15,093,075 bytes/s |
| block sequential read | 114,101,213 bytes/s |
| block random write | 6,642 IOPS |
| block random read | 2,647 IOPS |
| child create average / max | 556,833,721 / 632,059,048 ns |
| child start average / max | 1,154,472,915 / 1,274,576,134 ns |
| child destroy average / max | 328,868,335 / 394,533,464 ns |
| BusyBox heap / worker peak | 7,744 bytes / 4 workers |
| managed frame / TCB / Notification | 8,210 pages / 66 / 194 |

网络 RX 数值包含为确定性触发背压而设置的 100ms consumer pause，因此它是压力回归基线，不等同于
链路峰值带宽。顺序 read 也会受到 LKL page cache 影响；这些数值用于同配置回归比较。
