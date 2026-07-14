# Phase 1.2 — 稳定性、真实时间与自动回归

Phase 1.2 在 Phase 1.1 已跑通的 LKL-on-seL4 和交互 tty shell 基础上，完成了五项工程化工作：

1. 修复 host/shell 的边界正确性问题。
2. 让 LKL halt 和 root task 收尾不再触发次生 fault。
3. 提供真实单调时间和 oneshot timer。
4. 增加可判定成功/失败的 QEMU 自动 smoke test 与 GitHub Actions。
5. 增加固定版本依赖初始化、补丁幂等应用和环境审计。

## 正确性修复

- `sh_printf` 和 `lkl_printf` 按实际缓冲区容量截断，不再把 `vsnprintf` 的所需长度用于越界读取。
- LKL 派生线程的 tid 固定映射到可复用 slot，累计创建不再受单调 tid 超过 64 限制。
- 启动阶段顺序创建并 join 80 个 host 线程，同时验证复用 tid 的 TLS 数据会清空且 destructor
  恰好执行 80 次。
- 派生 TCB 的 fault cap 改为真正的 seL4 Endpoint，不再把 Notification 当作 fault endpoint。
- COM1 I/O ops、控制台 TCB configure/start 和 fd 0/1/2 设置均有失败检查。
- `pwd`、`touch` 和目录读取的错误/类型处理得到补强。

## 时间与 timer

pc99 默认 ltimer 路径会重复获取同一 IRQ。新实现不再申请该 IRQ：

1. 启动时通过 PIT 轮询校准 TSC 频率。
2. `host_time()` 使用校准后的 TSC 返回从 host 初始化开始的单调纳秒。
3. 独立 seL4 timer TCB 轮询原子 deadline，实现 LKL 所需的 oneshot callback。
4. callback 通过 `lkl_trigger_irq()` 注入 LKL clockevent IRQ。

QEMU 日志中的 Linux 时间戳现在持续前进；shell 的 `sleep 100` 实测约 100.4ms，而不再是全程
`0.000000`。`time` 命令可直接显示 host 单调纳秒值。

## 正常关机

旧实现中，`lkl_sys_halt()` 最后会 join 宿主根线程 tid 1，但根线程不是通过 `thread_create`
创建的，没有 join Notification，最终对 cap 0 执行接收并产生次生 fault。

当前实现将根 host task 的 join 定义为成功 no-op，关机顺序为：

```text
shell exit
  → stop COM1 polling TCB
  → lkl_sys_halt() / LKL threads_cleanup
  → lkl_cleanup()
  → stop timer TCB / free host fault endpoint
  → print LUNA_SHUTDOWN_OK
  → suspend root task
```

实跑中 `lkl_sys_halt()` 返回 0，且输出中没有 cap fault、VM fault 或 kernel panic。

## 自动验证

```sh
./run.sh --build-only
./tools/smoke-test.sh
```

smoke test 自动等待 shell prompt，并依次执行：

```text
<240 字符未知命令，覆盖格式化截断边界>
time
sleep 100
mkdir /smoke
write /smoke/msg timer-ok
cat /smoke/msg
free
exit
```

测试要求长格式化输出后 shell 仍可继续运行；单调时间必须大于 0，100ms sleep 实测值必须在
80ms 到 5s 之间；同时还要求出现线程复用、timer、文件内容、meminfo、halt 返回 0 和
`LUNA_SHUTDOWN_OK` 标记，同时明确拒绝 cap fault、VM fault、host panic 和 kernel panic。

`.github/workflows/smoke.yml` 在 push、pull request 和手工触发时从固定依赖开始执行同一流程。

## 干净克隆复现

```sh
./setup-deps.sh
./run.sh --build-only
./tools/smoke-test.sh
```

`setup-deps.sh` 固定 seL4 manifest commit 和 LKL base commit，幂等应用
`patches/lkl-tty.patch`，并检查 Git、repo、CMake、Ninja、QEMU、xmllint 与所需 Python 模块。
`--check-only` 可以在不修改 checkout 的情况下完成审计。

## 仍然存在的边界

- timer 使用 TSC deadline polling TCB，会消耗调度时间；后续可升级为无冲突的 HPET/MSI IRQ 服务。
- 32MiB host bump heap 不回收。
- semaphore/mutex 后端仍以 Notification 二值语义为主。
- LKL 与 seL4 root task 共享 VSpace/CSpace，没有故障和权限隔离。
- 尚无 virtio block、持久根文件系统和网络数据通路。
