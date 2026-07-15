/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bounded asynchronous virtio-blk backend for manager-owned persistent
 * storage. A child worker batches descriptors through the shared transfer
 * window; backing pages and their frame capabilities remain manager-private.
 */
#include "luna_lkl_task_host.h"

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm/syscalls.h>
#include <stdbool.h>
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
enum task_disk_batch_state {
    TASK_DISK_BATCH_FREE,
    TASK_DISK_BATCH_BUILDING,
    TASK_DISK_BATCH_READY,
    TASK_DISK_BATCH_PROCESSING,
    TASK_DISK_BATCH_DONE,
};

struct task_disk_local_batch {
    volatile int state;
    unsigned long long sequence;
    unsigned count;
    enum luna_isolation_event event[LUNA_DISK_BATCH_SLOTS];
    unsigned long long offset[LUNA_DISK_BATCH_SLOTS];
    unsigned char *buffer[LUNA_DISK_BATCH_SLOTS];
    unsigned char owned_data[LUNA_DISK_BATCH_SLOTS]
                            [LUNA_DISK_BATCH_SLOT_SIZE];
    size_t length[LUNA_DISK_BATCH_SLOTS];
    int result;
    int auto_release;
};

static struct task_disk_local_batch
    task_disk_batches[LUNA_DISK_LOCAL_QUEUE_COUNT];
static struct lkl_sem *task_disk_worker_sem;
static lkl_thread_t task_disk_worker_tid;
static volatile int task_disk_worker_stop;
static volatile unsigned long long task_disk_sequence;
static volatile unsigned long long task_disk_ipc;
static volatile unsigned long long task_disk_batches_completed;
static volatile unsigned long long task_disk_requests;
static volatile unsigned long long task_disk_copies;
static volatile unsigned long long task_disk_backpressure;
static volatile unsigned long long task_disk_queue_high_water;
static struct task_disk_local_batch *task_disk_pending_write;
static volatile int task_disk_write_lock_word;
static volatile unsigned task_disk_async_pending;
static volatile int task_disk_async_error;

static void task_disk_write_lock(void)
{
    while (__atomic_test_and_set(&task_disk_write_lock_word,
                                 __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void task_disk_write_unlock(void)
{
    __atomic_clear(&task_disk_write_lock_word, __ATOMIC_RELEASE);
}

static int task_disk_get_capacity(struct lkl_disk disk,
                                  unsigned long long *capacity)
{
    (void)disk;
    if (!capacity || !task_disk_io || !task_disk_size) return -1;
    *capacity = task_disk_size;
    return 0;
}

static int task_disk_read_direct(unsigned long long offset,
                                 unsigned char *buffer, size_t length)
{
    const size_t window = LUNA_DISK_IO_SIZE - LUNA_DISK_BATCH_DATA_OFFSET;
    while (length) {
        size_t chunk = length < window ? length : window;
        luna_lkl_task_manager_lock();
        int result = luna_lkl_task_manager_request(
            LUNA_ISOLATION_EVENT_DISK_READ, (seL4_Word)offset,
            (seL4_Word)chunk);
        if (!result)
            memcpy(buffer, task_disk_io + LUNA_DISK_BATCH_DATA_OFFSET,
                   chunk);
        luna_lkl_task_manager_unlock();
        __atomic_fetch_add(&task_disk_ipc, 1ULL, __ATOMIC_RELAXED);
        if (result) return -1;
        __atomic_fetch_add(&task_disk_requests, 1ULL, __ATOMIC_RELAXED);
        __atomic_fetch_add(&task_disk_copies, 1ULL, __ATOMIC_RELAXED);
        buffer += chunk;
        offset += chunk;
        length -= chunk;
    }
    return 0;
}

static void task_disk_record_high_water(void)
{
    unsigned long long used = 0;
    for (unsigned i = 0; i < LUNA_DISK_LOCAL_QUEUE_COUNT; i++) {
        if (__atomic_load_n(&task_disk_batches[i].state,
                            __ATOMIC_ACQUIRE) != TASK_DISK_BATCH_FREE)
            used++;
    }
    unsigned long long high = __atomic_load_n(&task_disk_queue_high_water,
                                               __ATOMIC_RELAXED);
    while (used > high &&
           !__atomic_compare_exchange_n(&task_disk_queue_high_water, &high,
                                        used, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
}

static int task_disk_process_batch(struct task_disk_local_batch *batch)
{
    struct luna_disk_batch_header *header = (void *)task_disk_io;
    for (unsigned i = 0; i < batch->count; i++) {
        header->descriptors[i].event = batch->event[i];
        header->descriptors[i].offset = (seL4_Word)batch->offset[i];
        header->descriptors[i].length = (seL4_Word)batch->length[i];
        header->descriptors[i].status = (seL4_Word)-1;
        if (batch->event[i] == LUNA_ISOLATION_EVENT_DISK_WRITE) {
            memcpy(task_disk_io + LUNA_DISK_BATCH_DATA_OFFSET +
                       i * LUNA_DISK_BATCH_SLOT_SIZE,
                   batch->buffer[i], batch->length[i]);
            __atomic_fetch_add(&task_disk_copies, 1ULL, __ATOMIC_RELAXED);
        }
    }
    header->completed = 0;
    header->count = batch->count;
    __sync_synchronize();
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request(
        LUNA_ISOLATION_EVENT_DISK_SUBMIT_BATCH, batch->count, 0);
    luna_lkl_task_manager_unlock();
    __atomic_fetch_add(&task_disk_ipc, 1ULL, __ATOMIC_RELAXED);
    if (result || header->completed != batch->count) return -1;
    for (unsigned i = 0; i < batch->count; i++) {
        if ((int)header->descriptors[i].status) return -1;
        if (batch->event[i] == LUNA_ISOLATION_EVENT_DISK_READ) {
            memcpy(batch->buffer[i],
                   task_disk_io + LUNA_DISK_BATCH_DATA_OFFSET +
                       i * LUNA_DISK_BATCH_SLOT_SIZE,
                   batch->length[i]);
            __atomic_fetch_add(&task_disk_copies, 1ULL, __ATOMIC_RELAXED);
        }
    }
    __atomic_fetch_add(&task_disk_batches_completed, 1ULL,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&task_disk_requests, batch->count,
                       __ATOMIC_RELAXED);
    return 0;
}

static void task_disk_worker(void *argument)
{
    (void)argument;
    for (;;) {
        lkl_host_ops.sem_down(task_disk_worker_sem);
        struct task_disk_local_batch *selected = NULL;
        unsigned long long sequence = ~0ULL;
        for (unsigned i = 0; i < LUNA_DISK_LOCAL_QUEUE_COUNT; i++) {
            struct task_disk_local_batch *batch = &task_disk_batches[i];
            if (__atomic_load_n(&batch->state, __ATOMIC_ACQUIRE) ==
                    TASK_DISK_BATCH_READY && batch->sequence < sequence) {
                selected = batch;
                sequence = batch->sequence;
            }
        }
        if (!selected) {
            if (__atomic_load_n(&task_disk_worker_stop, __ATOMIC_ACQUIRE))
                break;
            continue;
        }
        int expected = TASK_DISK_BATCH_READY;
        if (!__atomic_compare_exchange_n(&selected->state, &expected,
                                         TASK_DISK_BATCH_PROCESSING, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE))
            continue;
        selected->result = task_disk_process_batch(selected);
        if (selected->auto_release) {
            if (selected->result)
                __atomic_store_n(&task_disk_async_error, 1,
                                 __ATOMIC_RELEASE);
            selected->count = 0;
            selected->auto_release = 0;
            __atomic_store_n(&selected->state, TASK_DISK_BATCH_FREE,
                             __ATOMIC_RELEASE);
            __atomic_fetch_sub(&task_disk_async_pending, 1U,
                               __ATOMIC_ACQ_REL);
        } else {
            __atomic_store_n(&selected->state, TASK_DISK_BATCH_DONE,
                             __ATOMIC_RELEASE);
        }
    }
}

static struct task_disk_local_batch *task_disk_claim_batch(void)
{
    for (unsigned attempt = 0; attempt < 100000U; attempt++) {
        for (unsigned i = 0; i < LUNA_DISK_LOCAL_QUEUE_COUNT; i++) {
            int expected = TASK_DISK_BATCH_FREE;
            if (__atomic_compare_exchange_n(&task_disk_batches[i].state,
                                             &expected,
                                             TASK_DISK_BATCH_BUILDING,
                                             false, __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                task_disk_record_high_water();
                return &task_disk_batches[i];
            }
        }
        __atomic_fetch_add(&task_disk_backpressure, 1ULL,
                           __ATOMIC_RELAXED);
        seL4_Yield();
    }
    return NULL;
}

static void task_disk_publish_batch(struct task_disk_local_batch *batch,
                                    int auto_release)
{
    batch->sequence = __atomic_fetch_add(&task_disk_sequence, 1ULL,
                                          __ATOMIC_RELAXED);
    batch->result = -1;
    batch->auto_release = auto_release;
    if (auto_release)
        __atomic_fetch_add(&task_disk_async_pending, 1U,
                           __ATOMIC_ACQ_REL);
    __atomic_store_n(&batch->state, TASK_DISK_BATCH_READY,
                     __ATOMIC_RELEASE);
    lkl_host_ops.sem_up(task_disk_worker_sem);
}

static int task_disk_submit_batch(struct task_disk_local_batch *batch)
{
    task_disk_publish_batch(batch, 0);
    while (__atomic_load_n(&batch->state, __ATOMIC_ACQUIRE) !=
           TASK_DISK_BATCH_DONE)
        seL4_Yield();
    int result = batch->result;
    batch->count = 0;
    __atomic_store_n(&batch->state, TASK_DISK_BATCH_FREE,
                     __ATOMIC_RELEASE);
    return result;
}

static int task_disk_flush_writes(void)
{
    task_disk_write_lock();
    struct task_disk_local_batch *batch = task_disk_pending_write;
    task_disk_pending_write = NULL;
    if (batch && batch->count) task_disk_publish_batch(batch, 1);
    task_disk_write_unlock();
    while (__atomic_load_n(&task_disk_async_pending, __ATOMIC_ACQUIRE))
        seL4_Yield();
    return __atomic_load_n(&task_disk_async_error, __ATOMIC_ACQUIRE) ?
           -1 : 0;
}

static int task_disk_queue_write(struct lkl_blk_req *request,
                                 unsigned long long offset)
{
    task_disk_write_lock();
    for (int i = 0; i < request->count; i++) {
        size_t iov_offset = 0;
        while (iov_offset < request->buf[i].iov_len) {
            if (!task_disk_pending_write) {
                task_disk_pending_write = task_disk_claim_batch();
                if (!task_disk_pending_write) {
                    task_disk_write_unlock();
                    return -1;
                }
                task_disk_pending_write->count = 0;
            }
            struct task_disk_local_batch *batch = task_disk_pending_write;
            size_t remaining = request->buf[i].iov_len - iov_offset;
            size_t chunk = remaining < LUNA_DISK_BATCH_SLOT_SIZE ?
                           remaining : LUNA_DISK_BATCH_SLOT_SIZE;
            unsigned slot = batch->count++;
            batch->event[slot] = LUNA_ISOLATION_EVENT_DISK_WRITE;
            batch->offset[slot] = offset;
            batch->buffer[slot] = batch->owned_data[slot];
            batch->length[slot] = chunk;
            memcpy(batch->owned_data[slot],
                   (unsigned char *)request->buf[i].iov_base + iov_offset,
                   chunk);
            __atomic_fetch_add(&task_disk_copies, 1ULL, __ATOMIC_RELAXED);
            offset += chunk;
            iov_offset += chunk;
            if (batch->count == LUNA_DISK_BATCH_SLOTS) {
                task_disk_pending_write = NULL;
                task_disk_publish_batch(batch, 1);
            }
        }
    }
    task_disk_write_unlock();
    return __atomic_load_n(&task_disk_async_error, __ATOMIC_ACQUIRE) ?
           -1 : 0;
}

static int task_disk_request(struct lkl_disk disk, struct lkl_blk_req *request)
{
    (void)disk;
    if (!request || !task_disk_io || request->count < 0)
        return LKL_DEV_BLK_STATUS_IOERR;

    if (request->type == LKL_DEV_BLK_TYPE_FLUSH ||
        request->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
        if (task_disk_flush_writes()) return LKL_DEV_BLK_STATUS_IOERR;
        struct task_disk_local_batch *batch = task_disk_claim_batch();
        if (!batch) return LKL_DEV_BLK_STATUS_IOERR;
        batch->count = 1;
        batch->event[0] = LUNA_ISOLATION_EVENT_DISK_FLUSH;
        batch->offset[0] = 0;
        batch->buffer[0] = NULL;
        batch->length[0] = 0;
        int result = task_disk_submit_batch(batch);
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
    if (event == LUNA_ISOLATION_EVENT_DISK_WRITE)
        return task_disk_queue_write(request, offset) ?
               LKL_DEV_BLK_STATUS_IOERR : LKL_DEV_BLK_STATUS_OK;
    if (task_disk_flush_writes()) return LKL_DEV_BLK_STATUS_IOERR;
    for (int i = 0; i < request->count; i++) {
        if (task_disk_read_direct(offset, request->buf[i].iov_base,
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
    memset(task_disk_batches, 0, sizeof(task_disk_batches));
    task_disk_worker_sem = NULL;
    task_disk_worker_tid = 0;
    task_disk_worker_stop = 0;
    task_disk_sequence = 1;
    task_disk_ipc = 0;
    task_disk_batches_completed = 0;
    task_disk_requests = 0;
    task_disk_copies = 0;
    task_disk_backpressure = 0;
    task_disk_queue_high_water = 0;
    task_disk_pending_write = NULL;
    task_disk_write_lock_word = 0;
    task_disk_async_pending = 0;
    task_disk_async_error = 0;
    memset(&task_disk, 0, sizeof(task_disk));
    return 0;
}

int luna_lkl_task_disk_add(void)
{
    if (!task_disk_io || !task_disk_size || task_disk_id >= 0) return -1;
    task_disk_worker_sem = lkl_host_ops.sem_alloc(0);
    if (!task_disk_worker_sem) return -1;
    task_disk_worker_tid = lkl_host_ops.thread_create(task_disk_worker, NULL);
    if (!task_disk_worker_tid) {
        lkl_host_ops.sem_free(task_disk_worker_sem);
        task_disk_worker_sem = NULL;
        return -1;
    }
    task_disk.ops = &lkl_dev_blk_ops;
    task_disk_id = lkl_disk_add(&task_disk);
    if (task_disk_id < 0) {
        lkl_printf("luna-lkl-task: virtio block add failed: %d\n",
                   task_disk_id);
        __atomic_store_n(&task_disk_worker_stop, 1, __ATOMIC_RELEASE);
        lkl_host_ops.sem_up(task_disk_worker_sem);
        lkl_host_ops.thread_join(task_disk_worker_tid);
        lkl_host_ops.sem_free(task_disk_worker_sem);
        task_disk_worker_sem = NULL;
        task_disk_worker_tid = 0;
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
    if (task_disk_flush_writes()) return -1;
    lkl_sys_sync();
    if (task_disk_flush_writes()) return -1;
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
    if (task_disk_flush_writes()) return -1;
    int result = lkl_disk_remove(task_disk);
    if (result < 0) {
        lkl_printf("luna-lkl-task: post-halt disk cleanup failed: %d\n",
                   result);
        return -1;
    }
    task_disk_id = -1;
    memset(&task_disk, 0, sizeof(task_disk));
    __atomic_store_n(&task_disk_worker_stop, 1, __ATOMIC_RELEASE);
    lkl_host_ops.sem_up(task_disk_worker_sem);
    if (lkl_host_ops.thread_join(task_disk_worker_tid)) return -1;
    lkl_host_ops.sem_free(task_disk_worker_sem);
    task_disk_worker_sem = NULL;
    task_disk_worker_tid = 0;
    lkl_printf("LUNA_DISK_BATCH_COUNTERS ipc=%llu batches=%llu "
               "requests=%llu copies=%llu backpressure=%llu "
               "queue_high_water=%llu\n",
               task_disk_ipc, task_disk_batches_completed,
               task_disk_requests, task_disk_copies,
               task_disk_backpressure, task_disk_queue_high_water);
    return 0;
}
