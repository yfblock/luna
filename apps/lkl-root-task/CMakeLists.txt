# SPDX-License-Identifier: GPL-2.0
# apps/lkl-root-task/CMakeLists.txt — root manager plus isolated LKL task.
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
set(BUSYBOX_DIR     "$ENV{BUSYBOX_DIR}" CACHE PATH "BusyBox 源树")
if(NOT LKL_KERNEL_OBJ OR NOT EXISTS "${LKL_KERNEL_OBJ}")
    message(FATAL_ERROR "LKL_KERNEL_OBJ 未设置或不存在：${LKL_KERNEL_OBJ}")
endif()
if(NOT BUSYBOX_DIR OR NOT EXISTS "${BUSYBOX_DIR}/Makefile")
    message(FATAL_ERROR "BUSYBOX_DIR 未设置或不存在：${BUSYBOX_DIR}")
endif()

set(LUNA_BUSYBOX_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/luna-busybox.o")
set(LUNA_BUSYBOX_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/build-busybox-object.sh")
set(LUNA_BUSYBOX_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/../../configs/busybox_phase2_4.config")
add_custom_command(
    OUTPUT "${LUNA_BUSYBOX_OBJECT}"
    COMMAND "${LUNA_BUSYBOX_TOOL}" "${BUSYBOX_DIR}"
            "${LUNA_BUSYBOX_CONFIG}" "${LUNA_BUSYBOX_OBJECT}"
    DEPENDS "${LUNA_BUSYBOX_TOOL}" "${LUNA_BUSYBOX_CONFIG}"
            "${BUSYBOX_DIR}/coreutils/cat.c"
            "${BUSYBOX_DIR}/coreutils/echo.c"
            "${BUSYBOX_DIR}/coreutils/cp.c"
            "${BUSYBOX_DIR}/coreutils/ls.c"
            "${BUSYBOX_DIR}/editors/cmp.c"
            "${BUSYBOX_DIR}/findutils/find.c"
            "${BUSYBOX_DIR}/findutils/grep.c"
            "${BUSYBOX_DIR}/shell/ash.c"
    COMMENT "Build BusyBox ash and nofork applet static host-program object"
    VERBATIM
)
set_source_files_properties("${LUNA_BUSYBOX_OBJECT}"
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

add_executable(luna-lkl-task
    src/isolation_child.c
    src/lkl_task_host.c
    src/lkl_task_disk.c
    src/lkl_task_net.c
    src/lkl_task_user.c
    src/shell.c
    "${LUNA_BUSYBOX_OBJECT}"
    ${LKL_LINUX_DIR}/tools/lkl/lib/fs.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/iomem.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/virtio.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/virtio_blk.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/virtio_net.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/net.c
    ${LKL_LINUX_DIR}/tools/lkl/lib/utils.c
)
target_include_directories(luna-lkl-task PRIVATE include)
target_include_directories(luna-lkl-task PRIVATE
    ${LKL_LINUX_DIR}/tools/lkl/include
)
target_compile_options(luna-lkl-task PRIVATE
    -O2 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing
    -include sys/types.h
)
target_compile_definitions(luna-lkl-task PRIVATE _GNU_SOURCE)
target_link_libraries(luna-lkl-task
    ${LKL_KERNEL_OBJ}
    sel4runtime sel4 muslc utils sel4muslcsys sel4utils
)

include(cpio)
set(LUNA_CHILD_CPIO_DIR "${CMAKE_CURRENT_BINARY_DIR}/child-cpio")
set(LUNA_CHILD_CPIO_ELF "${LUNA_CHILD_CPIO_DIR}/luna-lkl-task")
set(LUNA_ROOTFS_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/luna-rootfs.ext4")
set(LUNA_ROOTFS_SEED "${CMAKE_CURRENT_SOURCE_DIR}/rootfs")
set(LUNA_ROOTFS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/make-rootfs.sh")
find_program(LUNA_MKE2FS mke2fs)
find_program(LUNA_E2FSCK e2fsck)
if(NOT LUNA_MKE2FS OR NOT LUNA_E2FSCK)
    message(FATAL_ERROR "Phase 2.3 requires mke2fs and e2fsck")
endif()
add_custom_command(
    OUTPUT "${LUNA_ROOTFS_IMAGE}"
    COMMAND "${LUNA_ROOTFS_TOOL}" "${LUNA_ROOTFS_IMAGE}" "${LUNA_ROOTFS_SEED}"
    DEPENDS "${LUNA_ROOTFS_TOOL}" "${LUNA_ROOTFS_SEED}/etc/luna-release"
            "${LUNA_ROOTFS_SEED}/bin/busybox"
    COMMENT "Create checked 16MiB ext4 persistent rootfs image"
    VERBATIM
)
add_custom_target(luna-rootfs-image ALL DEPENDS "${LUNA_ROOTFS_IMAGE}")
add_custom_command(
    OUTPUT "${LUNA_CHILD_CPIO_ELF}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LUNA_CHILD_CPIO_DIR}"
    COMMAND ${CMAKE_OBJCOPY} --strip-debug
            "$<TARGET_FILE:luna-lkl-task>" "${LUNA_CHILD_CPIO_ELF}"
    DEPENDS luna-lkl-task
    COMMENT "Strip debug sections from embedded isolation child"
    VERBATIM
)
set(LUNA_BOOT_ARCHIVE_FILES "${LUNA_CHILD_CPIO_ELF}")
MakeCPIO(isolation-child-archive.o "${LUNA_BOOT_ARCHIVE_FILES}"
         DEPENDS luna-lkl-task)

add_executable(luna-lkl
    isolation-child-archive.o
    src/main.c
    src/isolation_manager.c
    src/isolation_net.c
)

target_include_directories(luna-lkl PRIVATE
    include
)

target_compile_definitions(luna-lkl PRIVATE
    CONFIG_DEBUG_BUILD          # seL4_DebugPutChar / DebugHalt
    _GNU_SOURCE
)
target_compile_options(luna-lkl PRIVATE -O2 -g -Wall)

# Root manager does not link LKL; the kernel object is owned by luna-lkl-task.
target_link_libraries(luna-lkl
    sel4runtime sel4 muslc utils
    sel4muslcsys sel4platsupport sel4utils sel4debug
    sel4allocman sel4simple sel4simple-default sel4vka sel4vspace
    ethdrivers
)

# 作为 root task
include(rootserver)
DeclareRootserver(luna-lkl)

include(simulation)
GenerateSimulateScript()
