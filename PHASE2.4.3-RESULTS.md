# Phase 2.4.3 — BusyBox nofork applet 扩展

Phase 2.4.3 在现有静态 BusyBox/ash ABI 上加入 8 个 BusyBox 原生 `APPLET_NOFORK` applet：

```text
mkdir rmdir unlink truncate basename dirname printenv uname
```

这些 applet 继续在唯一的交互 ash 静态程序上下文中运行，不引入伪 `fork/exec`，也不扩大 child 的
PCI、IRQ、DMA、VKA 或 Untyped capability。

## LKL syscall 边界

新增的 libc 符号重定向如下：

```text
mkdir    -> lkl_sys_mkdir
rmdir    -> lkl_sys_rmdir
chmod    -> lkl_sys_chmod
ftruncate -> lkl_sys_ftruncate
uname    -> lkl_syscall(__lkl__NR_uname)
```

`basename`、`dirname` 和 `printenv` 只使用现有内存、环境和标准输出路径；`unlink` 复用已有的
`lkl_sys_unlink` shim。`uname` 使用 Linux UAPI 的六个 65-byte 字段作为 wire layout，再复制到
程序侧 `struct utsname`，因此不会返回 seL4 宿主的内核信息。

## 持久与易失文件系统语义

自动交互回归在持久 ext4 rootfs 上执行：

```sh
mkdir -m 700 -p /tmp/bb-dir/nested
echo payload > /tmp/bb-dir/nested/file
truncate -s 3 /tmp/bb-dir/nested/file
basename /tmp/bb-dir/nested/file
dirname /tmp/bb-dir/nested/file
printenv LUNA_APPLET_ENV
uname -s
```

LKL 当前在交互 ash 退出后直接 `sync + lkl_sys_halt()`，不能可靠地显式卸载仍被内核 task 引用的
ext4。若在持久盘上删除刚访问过的 inode，inode 可能仍在 dcache 中，halt 时 ext4 orphan list 尚未
完成清理，宿主 `e2fsck -fn` 会报告 orphan inode。为保持文件系统一致性门槛，shell 准备阶段把
内核已内置的 `ramfs` 挂载到 `/run`，并在该易失挂载上验证：

```sh
mkdir -p /run/bb-remove/nested
echo remove-me > /run/bb-remove/nested/file
unlink /run/bb-remove/nested/file
rmdir /run/bb-remove/nested /run/bb-remove
test ! -e /run/bb-remove
```

这仍然经过 BusyBox applet、静态 ABI 和 LKL VFS，只是不把未回收的删除 inode 写入 host-file ext4。

## 验收结果

正式回归要求每个新增 applet 的结果作为独立输出行出现，并继续要求：

```text
LUNA_RESTART_STRESS_OK rounds=100
LUNA_NET_PEER_TX_COMPLETE unique=2048 count=2048
LUNA_NETWORK_IRQ_OK ... fallback_polls=0
LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0
busybox-applets-ok
SMOKE TEST PASSED
HOST FILE FSCK PASSED
```

验证命令：

```sh
./run.sh --build-only
python3 -m py_compile tools/qemu_smoke.py
bash -n run.sh tools/build-busybox-object.sh tools/smoke-test.sh
./tools/smoke-test.sh --cross-qemu --timeout 480
```

当前边界不变：pipeline、后台任务和通用外部程序仍需要独立的静态 process/pipe runtime；任意
`fopen/fdopen FILE*` 也仍未纳入静态 ABI。
