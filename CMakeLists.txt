# SPDX-License-Identifier: GPL-2.0
# CMakeLists.txt — luna 顶层（Phase 1.1）
#
# 实际构建不走此文件。root task 的构建由 apps/lkl-root-task/CMakeLists.txt +
# deps/lkl_settings.cmake 直接驱动（见 README.md / PHASE1-RESULTS.md “复现”）。
#
# 依赖源树（deps/，由 seL4 manifest + lkl/linux fork 提供，不在本仓库管理）：
#   seL4 kernel, seL4_tools, util_libs, musllibc, sel4runtime, seL4_libs,
#   capdl, global-components, sel4-tutorials  +  lkl-linux
cmake_minimum_required(VERSION 3.13)
project(luna C ASM)
message(STATUS "luna Phase 1.1 — 见 README.md / PHASE1-RESULTS.md 的复现步骤。"
                " 实际构建：cmake -G Ninja -B deps/build_lkl -S apps/lkl-root-task "
                "（需 -DLUNA_SETTINGS / LKL_LINUX_DIR / LKL_KERNEL_OBJ 由环境变量提供）")
