# Phase 2.4 — 静态用户程序 ABI 与 BusyBox

Phase 2.4 在持久 ext4 根文件系统中加入 `/bin/busybox`，并由隔离的 `luna-lkl-task` 启动最小
BusyBox 1.36.1。自动回归真实执行：

```sh
busybox sh -c 'echo ok > /tmp/x; cat /tmp/x'
```

输出 `ok` 后，host program thread 正常退出、join 并归还 thread slot；原有 100 轮 LKL child
boot/mount/unmount/halt/destroy 与持久化回归继续通过。

## LKL 用户 ABI 边界

该版本 LKL 不是普通 Linux 用户态执行环境：

- `CONFIG_MMU` 未启用；
- `arch/lkl/include/asm/elf.h` 的 `elf_check_arch()` 固定拒绝 ELF；
- `start_thread()` 为空；
- LKL 的 init 是调用 `lkl_start_kernel()` 的宿主线程。

因此本阶段没有把 BusyBox 伪装成 LKL 内部的 ELF 进程。BusyBox 的选定 applet 作为静态 host-program
对象链接进隔离 child，持久 rootfs 中的 `/bin/busybox` 是版本化的 `LUNA-STATIC-ABI-1` 程序描述。
loader 校验该描述后，通过 BusyBox 的 `lbb_main()` 初始化 applet 表与运行时，再分派到 `ash`。

## 构建与依赖

`setup-deps.sh` 固定 BusyBox 1.36.1 commit：

```text
1a64f6a20aaf6ea4dbba68bbfa8cc1ab7e5c57c4
```

最小配置只启用 BusyBox library、`ash`、shell `echo` 与 `cat`。`cat` 通过仓库补丁标记为 nofork
applet；该 applet 只读取并复制 fd，不需要进程私有全局状态。`tools/build-busybox-object.sh` 在独立
build 目录生成约 64KiB relocatable object，并使用 `objcopy` 将文件、errno、内存、信号和进程接口
重定向到 Luna 静态 ABI。构建会拒绝残留的原生 `open/read/write/fork/execve/waitpid` 符号。

## syscall shim 与 fd 继承

BusyBox program thread 继承 clean child 已打开的 LKL fd 0/1/2。以下接口转入 LKL：

- `open/close/read/write/lseek`；
- `dup/dup2/fcntl`，用于 shell 重定向保存和恢复 stdout；
- `stat/lstat/fstat`，包含 LKL stat 到 musl stat 的显式字段转换；
- `chdir/fchdir/chroot/getcwd/umask/unlink/rename`。

BusyBox errno 使用程序实例自己的存储，不依赖 seL4 musl 的 POSIX pthread TLS。动态内存来自 child
内一个 1MiB、16-byte 对齐、整次执行后整体丢弃的 bounded arena。非交互 `ash -c` 所需的信号接口
提供受控兼容语义；`prctl` name 设置被安全忽略。

## spawn/wait 与禁止路径

launcher 使用 Phase 2.2 的固定 host thread pool 创建一个 program thread，并用同一 join 路径等待。
BusyBox `_exit/exit` 被转换为同线程 `longjmp` 回到 launcher，随后 thread trampoline 执行 TLS
destructor、发出 join Notification 并归还 slot。

这不是 Linux `fork/exec`。`fork/vfork/execve/execvp/waitpid/pipe` 在当前静态 ABI 中显式返回
`ENOSYS` 并增加 violation counter；验收命令若触发任何一个禁止接口，smoke 会失败。BusyBox shell
的 `echo` 是 builtin，`cat` 是 nofork applet，因此本阶段的命令在一个受控 program context 内完成。

## 验证

```sh
./setup-deps.sh --check-only
./run.sh --build-only
./tools/smoke-test.sh --timeout 480
```

关键输出：

```text
LUNA_RESTART_STRESS_OK rounds=100
LUNA_PERSISTENCE_OK rounds=100
ok
LUNA_STATIC_USER_OK path=/bin/busybox abi=1
LUNA_SPAWN_WAIT_OK pid=173 status=0
LUNA_BUSYBOX_OK command=echo ok > /tmp/x; cat /tmp/x
LUNA_PHASE2_4_USER_OK
LUNA_SHUTDOWN_OK
SMOKE TEST PASSED
```

## 后续

Phase 2.4 的验收门槛已完成。继续扩展 BusyBox 时，应优先增加能在同一静态 context 中安全运行的
builtin/nofork applet；真正的独立 ELF、地址空间复制和 POSIX `fork/exec` 需要引入 LKL 之外的
seL4 process loader/runtime，不能建立在当前 LKL NOMMU arch 的空 `start_thread()` 上。下一阶段进入
Phase 2.5 virtio-net 数据通路。
