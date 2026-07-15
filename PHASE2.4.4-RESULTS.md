# Phase 2.4.4 — 受控静态 process/pipe runtime

Phase 2.4.4 在不伪造 LKL native ELF 进程的前提下，为 BusyBox ash 增加了受控的静态外部 applet
runtime。它支持最多 4 个并发 worker、2–4 段 pipeline、单 applet 后台执行和 `wait`，同时保持
child 的 PCI、IRQ、DMA、VKA 与 Untyped capability 集合不变。

## Worker 隔离与生命周期

构建阶段从 BusyBox 1.36.1 的 nofork applet 和 libbb 生成 worker object，再通过符号前缀构建 4 份
独立副本。每个 slot 因此拥有独立的 applet/libbb 可写全局状态；跨 worker 共享的 Luna 1MiB
bounded heap 使用自旋锁保护，glibc getopt 的入口另行串行化并把 `optind/optarg` 保存回对应 slot。

每个 worker 使用 Phase 2.2 固定 thread pool 启动，复制 argv，并维护独立的虚拟 fd 0/1/2 映射。
退出通过 slot-local `setjmp/longjmp` 返回 launcher，随后关闭 fd、join thread 并释放 slot。公开 pid
使用 LKL host thread 的唯一 tid，避免可复用 slot 引入 ABA。

## Pipeline 与后台任务

ash 对可支持的静态命令调用 Luna runtime：

- 普通外部 applet：spawn 后同步 join，并返回 applet exit status。
- pipeline：创建 LKL `pipe2`，为每段复制正确的输入输出端，关闭 launcher 和 worker 的多余引用，
  等待全部 worker，并使用最后一段状态作为 pipeline 状态。
- 后台任务：记录 ash job/pid 后立即返回；worker 完成时发送受控 SIGCHLD 通知，`waitpid` 完成 join
  和资源回收。

自动回归覆盖：

```sh
printf 'pipeline-ok\n' | cat
echo pipeline-three-ok | cat | cat
true | false || echo pipeline-status-ok
echo background-ok & wait; echo background-wait-ok
```

runtime 最终报告：

```text
LUNA_STATIC_RUNTIME_OK workers=13 pipelines=3 background=1
LUNA_BUSYBOX_INTERACTIVE_OK status=0 forbidden=0
```

## LKL-aware FILE subset 与 applet

静态 ABI 现在支持任意 LKL 文件的：

```text
fopen fdopen fclose fileno
getc fgetc fread fwrite fgets
feof ferror clearerr ungetc
fseek ftell rewind
```

pseudo `FILE*` 保存 LKL fd、ownership、EOF/error 和单字符 pushback 状态；标准流继续解析到 worker
虚拟 fd。`open()` 还保持 worker 虚拟命名空间中的 lowest-free-fd 规则，因此 `uniq FILE` 关闭 stdin
后重新打开输入文件仍得到虚拟 fd 0。

新增并验证的 BusyBox applet 包括：

```text
echo false ln printf readlink realpath touch true
head wc cut uniq
```

`touch/ln/readlink/realpath` 使用 LKL `utimensat/futimens`、link/symlink、readlink 和受控路径解析；
`head/wc/cut/uniq` 验证任意文件 stdio 和 getopt 状态隔离。

## 架构边界与验收

该 runtime 不是通用 POSIX process 模拟。当前 LKL NOMMU arch 仍拒绝 native ELF，`fork/vfork/exec`
shim 继续返回 `ENOSYS`；ash 只允许白名单静态 nofork applet，pipeline 最多 4 段，后台路径只支持
单个无重定向静态 applet，作业控制保持关闭。

正式验收命令：

```sh
./setup-deps.sh --check-only
./run.sh --build-only
./tools/smoke-test.sh --cross-qemu --timeout 480
```

验收继续要求 100 轮 child boot/halt/destroy、网络 RX/TX 压力、真实 virtio-net IRQ、跨 QEMU ext4
持久化和宿主 `e2fsck -fn` 全部通过。UINTR 不属于本阶段。
