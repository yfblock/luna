# Phase 2.3.1 — 宿主文件 ext4 backing 与跨 QEMU 重启持久化

Phase 2.3.1 将原先只存在于 manager RAM 的 16MiB ext4 backing 改为真正的宿主文件。`run.sh` 在首次
运行时从构建生成的 `luna-rootfs.ext4` 创建状态文件，随后 QEMU 使用：

```text
memory-backend-file,share=on + ivshmem-plain PCI 00:06.0
```

manager 独占 ivshmem PCI 配置空间和 16MiB MMIO BAR 映射。child 的设备模型和 capability 边界不变：
它仍通过 LKL virtio-blk、受控 Endpoint 和 64KiB shared window 请求读写，不能直接映射 backing，
也不持有 PCI、MMIO、VKA 或 Untyped capability。

默认交互运行使用 `deps/build_lkl/luna-rootfs-state.ext4`，因此关闭并重新启动 QEMU 后继续使用同一
文件。设置 `LUNA_DISK_IMAGE=/path/to/rootfs.ext4` 可选择状态文件，设置 `LUNA_DISK_RESET=1` 可从
构建 seed 重置。

跨 QEMU 回归使用同一个临时文件执行两次完整启动：第一次写入
`/qemu-power-persist`，正常 sync/halt 并退出 QEMU；第二次启动后读回：

```text
luna-cross-qemu-persist-ok
```

两次启动各自仍完成 100 轮 child 重建、TX/RX 网络压力、BusyBox 和 shell 回归，结束后对宿主 ext4
运行 `e2fsck -fn`。

复现：

```sh
./run.sh --build-only
./tools/smoke-test.sh --cross-qemu --timeout 480
```
