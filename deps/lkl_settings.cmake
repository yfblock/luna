# SPDX-License-Identifier: GPL-2.0
# deps/lkl_settings.cmake — 给 luna LKL root task 用的 seL4 构建设置（非 tutorial）。
# 基于 sel4-tutorials/settings.cmake，去掉 tutorial 生成步骤，固定 x86_64 pc99。
include_guard(GLOBAL)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
file(GLOB project_modules ${project_dir}/projects/*)
list(APPEND CMAKE_MODULE_PATH
     ${project_dir}/kernel
     ${project_dir}/tools/seL4/cmake-tool/helpers/
     ${project_dir}/tools/seL4/elfloader-tool/
     ${project_modules})
set(POLLY_DIR ${project_dir}/tools/polly CACHE INTERNAL "")

include(application_settings)

set(KernelArch "x86" CACHE STRING "" FORCE)
set(KernelPlatform "pc99" CACHE STRING "" FORCE)
set(KernelSel4Arch "x86_64" CACHE STRING "" FORCE)

include(${project_dir}/kernel/configs/seL4Config.cmake)
set(CapDLLoaderMaxObjects 20000 CACHE STRING "" FORCE)
set(KernelRootCNodeSizeBits 16 CACHE STRING "")

# 用 kernel debug putchar 做控制台（与 LKL host_ops.print 一致）
set(LibSel4PlatSupportUseDebugPutChar true CACHE BOOL "" FORCE)
set(LibSel4MuslcSysDebugHalt FALSE CACHE BOOL "" FORCE)
set(KernelNumDomains 1 CACHE STRING "" FORCE)

ApplyCommonReleaseVerificationSettings(FALSE FALSE)
ApplyCommonSimulationSettings(${KernelSel4Arch})
