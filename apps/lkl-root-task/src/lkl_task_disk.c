/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synchronous virtio-blk backend for manager-owned persistent storage. Only a
 * bounded shared transfer window is mapped into the child; the disk backing
 * pages and their frame capabilities remain private to the manager.
 */
#include "luna_lkl_task_host.h"

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm/syscalls.h>
#include <stdint.h>
#include <string.h>

#define TASK_DISK_SECTOR_SIZE 512ULL
#define TASK_DISK_DEVICE_PATH "/dev/luna-root"
#define TASK_DISK_MOUNT_PATH  "/persist"
#define TASK_ROOTFS_RELEASE   "luna Phase 2.3 persistent rootfs\n"

static unsigned char *task_disk_io;
static unsigned long task_disk_io_size;
static unsigned long task_disk_size;
static struct lkl_disk task_disk;
static int task_disk_id = -1;
static int task_old_root_fd = -1;

static int task_disk_get_capacity(struct lkl_disk disk,
                                  unsigned long long *capacity)
{
    (void)disk;
    if (!capacity || !task_disk_io || !task_disk_size) return -1;
    *capacity = task_disk_size;
    return 0;
}

static int task_disk_exchange(enum luna_isolation_event event,
                              unsigned long long offset,
                              unsigned char *buffer, size_t length)
{
    while (length) {
        size_t chunk = length < task_disk_io_size ? length :
                       task_disk_io_size;
        luna_lkl_task_manager_lock();
        if (event == LUNA_ISOLATION_EVENT_DISK_WRITE)
            memcpy(task_disk_io, buffer, chunk);
        int result = luna_lkl_task_manager_request(
            event, (seL4_Word)offset, (seL4_Word)chunk);
        if (!result && event == LUNA_ISOLATION_EVENT_DISK_READ)
            memcpy(buffer, task_disk_io, chunk);
        luna_lkl_task_manager_unlock();
        if (result) return -1;
        buffer += chunk;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}

static int task_disk_request(struct lkl_disk disk, struct lkl_blk_req *request)
{
    (void)disk;
    if (!request || !task_disk_io || request->count < 0)
        return LKL_DEV_BLK_STATUS_IOERR;

    if (request->type == LKL_DEV_BLK_TYPE_FLUSH ||
        request->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
        luna_lkl_task_manager_lock();
        int result = luna_lkl_task_manager_request(
            LUNA_ISOLATION_EVENT_DISK_FLUSH, 0, 0);
        luna_lkl_task_manager_unlock();
        return result ? LKL_DEV_BLK_STATUS_IOERR :
                        LKL_DEV_BLK_STATUS_OK;
    }
    if (request->type != LKL_DEV_BLK_TYPE_READ &&
        request->type != LKL_DEV_BLK_TYPE_WRITE)
        return LKL_DEV_BLK_STATUS_UNSUP;
    if (request->sector > task_disk_size / TASK_DISK_SECTOR_SIZE)
        return LKL_DEV_BLK_STATUS_IOERR;

    unsigned long long offset = request->sector * TASK_DISK_SECTOR_SIZE;
    unsigned long long total = 0;
    for (int i = 0; i < request->count; i++) {
        if (!request->buf[i].iov_base ||
            request->buf[i].iov_len > task_disk_size - total)
            return LKL_DEV_BLK_STATUS_IOERR;
        total += request->buf[i].iov_len;
    }
    if (offset > task_disk_size || total > task_disk_size - offset)
        return LKL_DEV_BLK_STATUS_IOERR;

    enum luna_isolation_event event =
        request->type == LKL_DEV_BLK_TYPE_READ ?
        LUNA_ISOLATION_EVENT_DISK_READ :
        LUNA_ISOLATION_EVENT_DISK_WRITE;
    for (int i = 0; i < request->count; i++) {
        if (task_disk_exchange(event, offset, request->buf[i].iov_base,
                               request->buf[i].iov_len))
            return LKL_DEV_BLK_STATUS_IOERR;
        offset += request->buf[i].iov_len;
    }
    return LKL_DEV_BLK_STATUS_OK;
}

struct lkl_dev_blk_ops lkl_dev_blk_ops = {
    .get_capacity = task_disk_get_capacity,
    .request = task_disk_request,
};

int luna_lkl_task_configure_disk(void *io_base, unsigned long io_size,
                                 unsigned long disk_size)
{
    if ((uintptr_t)io_base != LUNA_DISK_IO_BASE ||
        io_size != LUNA_DISK_IO_SIZE ||
        disk_size != LUNA_PERSISTENT_DISK_SIZE ||
        ((uintptr_t)io_base & (TASK_DISK_SECTOR_SIZE - 1)))
        return -1;
    task_disk_io = io_base;
    task_disk_io_size = io_size;
    task_disk_size = disk_size;
    task_disk_id = -1;
    task_old_root_fd = -1;
    memset(&task_disk, 0, sizeof(task_disk));
    return 0;
}

int luna_lkl_task_disk_add(void)
{
    if (!task_disk_io || !task_disk_size || task_disk_id >= 0) return -1;
    task_disk.ops = &lkl_dev_blk_ops;
    task_disk_id = lkl_disk_add(&task_disk);
    if (task_disk_id < 0) {
        lkl_printf("luna-lkl-task: virtio block add failed: %d\n",
                   task_disk_id);
        return -1;
    }
    return 0;
}

static int task_mkdir(const char *path, int mode)
{
    long result = lkl_sys_mkdir(path, mode);
    return result == 0 || result == -LKL_EEXIST ? 0 : (int)result;
}

static int task_read_exact(const char *path, const char *expected)
{
    char buffer[96];
    size_t expected_length = strlen(expected);
    long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
    if (fd < 0) return (int)fd;
    long length = lkl_sys_read((int)fd, buffer, sizeof(buffer));
    long close_result = lkl_sys_close((int)fd);
    if (length != (long)expected_length || close_result < 0 ||
        memcmp(buffer, expected, expected_length))
        return -LKL_EIO;
    return 0;
}

static int task_write_marker(void)
{
    const char marker[] = LUNA_PERSISTENT_MARKER;
    long fd = lkl_sys_open(TASK_DISK_MOUNT_PATH "/luna-persist",
                           LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0644);
    if (fd < 0) return (int)fd;
    long length = lkl_sys_write((int)fd, (void *)marker,
                                sizeof(marker) - 1);
    long sync_result = length == (long)(sizeof(marker) - 1) ?
        lkl_sys_fsync((unsigned int)fd) : -LKL_EIO;
    long close_result = lkl_sys_close((int)fd);
    if (length != (long)(sizeof(marker) - 1) || sync_result < 0 ||
        close_result < 0)
        return -LKL_EIO;
    return 0;
}

static int task_unmount_and_remove(void)
{
    long result = lkl_sys_umount(TASK_DISK_MOUNT_PATH, 0);
    if (result < 0) return (int)result;
    result = lkl_sys_unlink(TASK_DISK_DEVICE_PATH);
    if (result < 0) return (int)result;
    /* Keep the virtio block backend alive through lkl_sys_halt(). The ext4
     * shutdown path can still issue final superblock writes after umount;
     * removing the backend here turns those valid writes into I/O errors. */
    return 0;
}

int luna_lkl_task_disk_prepare(enum luna_isolation_mode mode)
{
    uint32_t device = 0;
    int result = lkl_get_virtio_blkdev(task_disk_id, 0, &device);
    if (result < 0) goto fail;
    if ((result = task_mkdir("/dev", 0755))) goto fail;
    long syscall_result = lkl_sys_mknod(TASK_DISK_DEVICE_PATH,
                                        LKL_S_IFBLK | 0600, device);
    if (syscall_result < 0 && syscall_result != -LKL_EEXIST) {
        result = (int)syscall_result;
        goto fail;
    }
    if ((result = task_mkdir(TASK_DISK_MOUNT_PATH, 0755))) goto fail;
    syscall_result = lkl_sys_mount(TASK_DISK_DEVICE_PATH,
                                   TASK_DISK_MOUNT_PATH, "ext4", 0, NULL);
    if (syscall_result < 0) {
        result = (int)syscall_result;
        goto fail;
    }
    result = task_read_exact(TASK_DISK_MOUNT_PATH "/etc/luna-release",
                             TASK_ROOTFS_RELEASE);
    if (result) goto fail;

    if (mode == LUNA_ISOLATION_MODE_FAULT)
        result = task_write_marker();
    else
        result = task_read_exact(TASK_DISK_MOUNT_PATH "/luna-persist",
                                 LUNA_PERSISTENT_MARKER);
    if (result) goto fail;
    lkl_sys_sync();

    if (mode == LUNA_ISOLATION_MODE_CLEAN) {
        if ((result = task_mkdir(TASK_DISK_MOUNT_PATH "/dev", 0755)) ||
            (result = task_mkdir(TASK_DISK_MOUNT_PATH "/proc", 0555)) ||
            (result = task_mkdir(TASK_DISK_MOUNT_PATH "/tmp", 01777)))
            goto fail;
        task_old_root_fd = (int)lkl_sys_open("/", LKL_O_RDONLY |
                                             LKL_O_DIRECTORY, 0);
        if (task_old_root_fd < 0 ||
            lkl_sys_chdir(TASK_DISK_MOUNT_PATH) < 0 ||
            lkl_sys_chroot(".") < 0 || lkl_sys_chdir("/") < 0) {
            result = -LKL_EIO;
            goto fail;
        }
        return 0;
    }

    return 0;

fail:
    lkl_printf("luna-lkl-task: persistent disk prepare failed: %d\n",
               result);
    return -1;
}

int luna_lkl_task_disk_finish(void)
{
    if (task_disk_id < 0) return 0;
    lkl_sys_sync();
    if (task_old_root_fd >= 0) {
        long proc_result = lkl_sys_umount("/proc", 0);
        if (proc_result < 0 && proc_result != -LKL_EINVAL) return -1;
        if (lkl_sys_close((unsigned int)task_old_root_fd) < 0) return -1;
        task_old_root_fd = -1;
        return 0;
    }
    int result = task_unmount_and_remove();
    if (result) {
        lkl_printf("luna-lkl-task: persistent disk finish failed: %d\n",
                   result);
        return -1;
    }
    return 0;
}

int luna_lkl_task_disk_cleanup_after_halt(void)
{
    if (task_disk_id < 0) return 0;
    int result = lkl_disk_remove(task_disk);
    if (result < 0) {
        lkl_printf("luna-lkl-task: post-halt disk cleanup failed: %d\n",
                   result);
        return -1;
    }
    task_disk_id = -1;
    memset(&task_disk, 0, sizeof(task_disk));
    return 0;
}
