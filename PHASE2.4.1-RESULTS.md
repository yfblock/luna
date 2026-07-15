# Phase 2.4.1 — 可交互 BusyBox ash 与可回收静态 ABI heap

Phase 2.4.1 让 clean child 的 `/dev/ttyLKL0` 直接进入 BusyBox 1.36.1 `ash -i`，不再先进入
Luna 自定义命令解释器。控制台 fd 0/1/2、文件重定向、当前目录和 nofork applet 的系统调用均进入
child 内的 LKL；root manager 仍不运行 LKL 或 BusyBox。

交互 shell 使用固定提示符：

```text
BusyBox v1.36.1 ... built-in shell (ash)
luna-ash#
```

当前自动回归覆盖：

```sh
echo busybox-interactive-ok
pwd
cd /tmp
echo timer-ok > /tmp/ash-msg
cat /tmp/ash-msg
test -f /tmp/ash-msg && echo busybox-test-ok
fsync /tmp/ash-msg
sync
sleep 1
cat /etc/luna-release
cat /proc/meminfo
exit
```

## ABI 与可靠性变化

- `isatty()`、`tcgetattr()` 和 `tcsetattr()` 转入 LKL tty ioctl；
- BusyBox nofork `sync` 直接调用 LKL `sync`；
- BusyBox nofork `fsync`/`fsync -d` 分别调用 LKL `fsync`/`fdatasync`；
- BusyBox `sleep()` 转入 LKL `nanosleep`，不再落到 seL4 musl 的宿主 syscall；
- 1MiB 静态 arena 从只增长、`free()` 为空改为 first-fit bounded allocator；
- allocator 支持 16-byte 对齐、split、相邻 free block 合并和保留内容的 `realloc()`；
- 启动前自测覆盖释放复用、对齐、realloc 数据保持和完整合并；
- 静态 applet spawn/join 验收与交互 `ash` 分离，使每个 child 只初始化一次 ash 全局状态；
- 删除旧 Luna 命令解释器，只保留 `/dev/ttyLKL0` 与 `/proc` 的准备代码。
- 网络持续吞吐回归对 UDP 数据包或 ACK 偶发丢失使用最多三轮有界重传；peer 仍要求收齐全部
  2048 个唯一序号后才确认完成。
- manager 的一次性 TX stress gate 先确定填满 16-entry software queue，并在第一次 retry 后自动
  放行，使背压测试不再依赖 IRQ drain 与 child send 的调度竞态。

作业控制显式关闭（`ash -i +m`）。当前 LKL NOMMU host-program ABI 仍不提供 native
`fork/exec/pipe/waitpid`，因此 pipeline、后台任务和非 nofork 外部 applet 尚未实现；这些调用仍以
`ENOSYS` 拒绝并进入 violation counter。

后续 Phase 2.4.2 已为 stdout/stderr 增加 LKL-aware stdio subset，并重新启用 `printf` builtin；
实现与边界见 `PHASE2.4.2-RESULTS.md`。

## 验证

```sh
ninja -C deps/build_lkl
./tools/smoke-test.sh --timeout 480
./tools/smoke-test.sh --cross-qemu --timeout 480
```

标准 QEMU 回归关键输出：

```text
LUNA_BUSYBOX_HEAP_OK bytes=1048576
LUNA_BUSYBOX_INTERACTIVE_READY prompt=luna-ash#
busybox-interactive-ok
busybox-test-ok
busybox-sleep-ok
LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0
LUNA_RESTART_STRESS_OK rounds=100
LUNA_NETWORK_IRQ_OK ... fallback_polls=0
SMOKE TEST PASSED
HOST FILE FSCK PASSED
```
