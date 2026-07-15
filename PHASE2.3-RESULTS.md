# Phase 2.3 — virtio-block 与持久根文件系统

Phase 2.3 为隔离的 `luna-lkl-task` 增加了 LKL virtio-mmio/block 设备，并将 16MiB ext4 镜像作为
clean child 交互 shell 的实际 `/`。同一 manager 生命周期内，文件系统状态跨 fault child、100 个
stress child 和最终 clean child 的完整销毁/重建保持不变。

## 所有权与隔离边界

完整磁盘 backing store 由 root manager 持有，位于 manager 私有 VSpace。child 不获得 backing frame
cap、VKA、Untyped、manager CSpace 或 manager VSpace。

manager 只把 64KiB 传输窗的 16 个 4KiB frame 临时映射到 child 的 `0x24000000`。这些 frame cap 的
副本仍位于 manager CSpace，由 child VSpace teardown 回收；child 只看到映射。每次 child 销毁后，
新的 child 会获得同一传输窗的新映射，而 16MiB backing store 保持不变。

```text
LKL virtio-blk request
  → child 将 write 数据复制到 64KiB window（read 则先请求 manager）
  → DISK_READ / DISK_WRITE / DISK_FLUSH(offset, length)
  → manager 校验 badge、范围和 window 上限
  → manager 在私有 16MiB backing 与 window 间复制
  → DISK_RESULT(status)
```

磁盘请求和 Phase 2.2 的 heap map/unmap 共用同一个 child manager-request lock，因此不同 LKL host
thread 不会交叉消费 command Endpoint 响应。

## LKL virtio-mmio backend

child 除 `lkl.o` 外，只编译 LKL 自带的 `iomem.c`、`virtio.c`、`virtio_blk.c` 和 `fs.c`；没有链接
POSIX host glue 或 hijack 库。`lkl_host_ops` 新增：

- `virtio_devices = lkl_virtio_devs`，在 kernel cmdline 注入 virtio-mmio 设备；
- `ioremap = lkl_ioremap` 与 `iomem_access = lkl_iomem_access`；
- 同步 block `get_capacity/request`，支持 READ、WRITE、FLUSH 和越界校验。

设备在 `lkl_init()` 后、`lkl_start_kernel()` 前通过 `lkl_disk_add()` 建立，因此内核启动时直接探测为
`vda`。fault/stress child 在同步卸载 ext4 后移除设备；clean child 以该文件系统为 chroot，先完成
sync 和 `/proc` 卸载，再 halt LKL，最后在 `lkl_is_running == false` 后释放 host virtio 对象。allocator
idle 断言仍要求所有 child host allocation 完整归零。

## ext4 镜像与历史稀疏打包

构建阶段由 `tools/make-rootfs.sh` 创建固定 UUID、无 journal 的 16MiB ext4 镜像，并立即运行
`e2fsck -fn`。seed rootfs 包含 `/etc/luna-release`。

Phase 2.3 最初直接把 backing 放在 manager RAM。为避免将 16MiB 原始镜像放入 CPIO，
`tools/pack-rootfs.py` 只打包
非零 4KiB block。manager 校验 magic、容量、block size、record 数和每个 block index 后，在私有
大页 backing 中重建完整镜像。当前 seed 的 pack 约 169KiB，原始 ext4 容量仍为 16MiB。

root allocman metadata pool 调整为 8MiB，并在固定 pool 与其他 root 静态状态之间保留 64KiB guard，
避免 mspace 边界元数据依赖链接器跨 translation unit 的 BSS 排序。

## 持久化与根切换测试

fault child 首次挂载 ext4，校验 `/etc/luna-release`，写入并 `fsync`：

```text
/luna-persist = luna-phase-2.3-persistent
```

随后每个 stress child 都重新探测 `vda`、挂载 ext4、读取并精确校验该文件，再 sync、卸载、移除设备、
halt 和销毁。100 轮结束后输出：

```text
LUNA_VIRTIO_BLOCK_OK bytes=16777216
LUNA_PERSISTENCE_OK rounds=100
```

最终 clean child 挂载同一 ext4，保留 bootstrap root fd 后执行 `chdir + chroot`，使 shell 的 `/` 成为
持久文件系统。自动 shell 回归读取 release 与跨 child marker，创建 `/smoke/msg`、执行 `sync`，再
完成正常 halt 和 post-halt virtio cleanup。

## 验证

```sh
./setup-deps.sh --check-only
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```

构建输出包含通过的 `e2fsck -fn` 五阶段检查。QEMU 关键输出：

```text
LUNA_VIRTIO_BLOCK_OK bytes=16777216
LUNA_RESTART_STRESS_OK rounds=100
LUNA_PERSISTENCE_OK rounds=100
LUNA_LKL_CHILD_SHELL_READY
luna Phase 2.3 persistent rootfs
luna-phase-2.3-persistent
synced
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

本阶段最初的“持久”边界是同一 seL4 manager 生命周期内的完整 LKL task 销毁/重建。Phase 2.3.1
已将 manager RAM backing 替换为 QEMU host-file ivshmem backing，并完成跨两次 QEMU 进程启动的
持久化验收；详见 `PHASE2.3.1-RESULTS.md`。

## 后续

Phase 2.3 的 LKL virtio-block、ext4 根切换和 managed-restart 持久化已完成。下一阶段进入 Phase 2.4：
在持久 rootfs 中运行静态用户程序和 BusyBox，并补齐 clone/fork/exec/wait 路径。
