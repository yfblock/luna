# Phase 2.4.2 — LKL-aware 标准流 stdio

Phase 2.4.2 补齐 BusyBox 静态 ABI 的 stdout/stderr 格式化输出。此前 ash redirection 只修改 LKL fd，
而 BusyBox `printf` 通过 seL4 musl stdio 写宿主 stdout，导致目标文件被创建但内容为空。本阶段重新
启用 `ASH_PRINTF`，并将标准流格式化接口直接落到 LKL fd 1/2。

当前 stdio subset 覆盖：

- `printf/vprintf/dprintf`；
- `fprintf/vfprintf`，限 `stdout` 与 `stderr`；
- `puts/putchar/fputs/putc` 及 unlocked 变体；
- 标准流 `fflush/ferror/clearerr`。

格式化先使用 `vsnprintf` 写入 512-byte 栈缓冲，超长输出使用 1MiB bounded heap；随后循环调用 LKL
`write` 处理 partial write。标准流为无缓冲直写，因此 `fflush(stdout/stderr)` 成功返回。任意由
`fopen/fdopen` 创建的 `FILE *` 尚未重定向，访问时返回 `ENOSYS`，不会静默落到宿主文件系统。

自动交互验证：

```sh
printf 'stdio-ok:%d\n' 7 > /tmp/stdio-msg
cat /tmp/stdio-msg
```

输出必须来自后续 `cat`：

```text
stdio-ok:7
```

并继续保留 BusyBox arena、100 轮生命周期、网络 IRQ/TX 背压、跨 QEMU ext4 持久化和 FSCK 门槛。

