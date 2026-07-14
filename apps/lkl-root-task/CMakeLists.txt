# SPDX-License-Identifier: GPL-2.0
# apps/lkl-root-task/CMakeLists.txt — seL4 root task hosting LKL.
#
# settings 文件须以 include() 在最开头加载（与 sel4-tutorials 一致），
# 这样 CMAKE_MODULE_PATH / kernel 配置在 find_package 前生效。
include($ENV{LUNA_SETTINGS})

cmake_minimum_required(VERSION 3.10)
project(luna_lkl C ASM)

find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)
find_package(musllibc REQUIRED)
find_package(util_libs REQUIRED)
find_package(seL4_libs REQUIRED)

sel4_import_kernel()
elfloader_import_project()
musllibc_setup_build_environment_with_sel4runtime()
sel4_import_libsel4()
util_libs_import_libraries()
sel4_libs_import_libraries()

# LKL 内核对象 + 头文件（已构建于 lkl-linux 树）
set(LKL_LINUX_DIR   "$ENV{LKL_LINUX_DIR}" CACHE PATH "lkl/linux 源树")
set(LKL_KERNEL_OBJ  "$ENV{LKL_KERNEL_OBJ}" CACHE FILEPATH "lkl.o 内核对象")
if(NOT LKL_KERNEL_OBJ OR NOT EXISTS "${LKL_KERNEL_OBJ}")
    message(FATAL_ERROR "LKL_KERNEL_OBJ 未设置或不存在：${LKL_KERNEL_OBJ}")
endif()

add_executable(luna-lkl-task
    src/isolation_child.c
)
target_include_directories(luna-lkl-task PRIVATE include)
target_include_directories(luna-lkl-task PRIVATE
    ${LKL_LINUX_DIR}/tools/lkl/include
)
target_compile_options(luna-lkl-task PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter
)
target_link_libraries(luna-lkl-task
    ${LKL_KERNEL_OBJ}
    sel4runtime sel4 muslc utils sel4muslcsys
)

include(cpio)
set(LUNA_CHILD_CPIO_DIR "${CMAKE_CURRENT_BINARY_DIR}/child-cpio")
set(LUNA_CHILD_CPIO_ELF "${LUNA_CHILD_CPIO_DIR}/luna-lkl-task")
add_custom_command(
    OUTPUT "${LUNA_CHILD_CPIO_ELF}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LUNA_CHILD_CPIO_DIR}"
    COMMAND ${CMAKE_OBJCOPY} --strip-debug
            "$<TARGET_FILE:luna-lkl-task>" "${LUNA_CHILD_CPIO_ELF}"
    DEPENDS luna-lkl-task
    COMMENT "Strip debug sections from embedded isolation child"
    VERBATIM
)
MakeCPIO(isolation-child-archive.o "${LUNA_CHILD_CPIO_ELF}"
         DEPENDS luna-lkl-task)

add_executable(luna-lkl
    isolation-child-archive.o
    src/main.c
    src/isolation_manager.c
    src/lkl_host_ops.c
    src/shell.c
)

target_include_directories(luna-lkl PRIVATE
    include
    ${LKL_LINUX_DIR}/tools/lkl/include
)

target_compile_definitions(luna-lkl PRIVATE
    CONFIG_DEBUG_BUILD          # seL4_DebugPutChar / DebugHalt
    _GNU_SOURCE
)
target_compile_options(luna-lkl PRIVATE -O2 -g -Wall)

# 链接 LKL 内核对象（.o 直接进链接）+ seL4 用户态库
target_link_libraries(luna-lkl
    ${LKL_KERNEL_OBJ}
    sel4runtime sel4 muslc utils
    sel4muslcsys sel4platsupport sel4utils sel4debug
    sel4allocman sel4simple sel4simple-default sel4vka sel4vspace
    sel4vka sel4utils
)

# 作为 root task
include(rootserver)
DeclareRootserver(luna-lkl)

include(simulation)
GenerateSimulateScript()
