/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Phase 2.4 static host-program ABI. LKL has no ELF userspace transition:
 * CONFIG_MMU is disabled, elf_check_arch() rejects binaries and start_thread()
 * is empty. BusyBox is therefore linked as host code in the isolated child,
 * while its file-descriptor syscalls are redirected into LKL.
 */
#include "luna_lkl_task_host.h"

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm/syscalls.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define LUNA_BUSYBOX_PATH "/bin/busybox"
#define LUNA_BUSYBOX_MANIFEST "LUNA-STATIC-ABI-1\nbusybox-1.36.1\n"
#define LUNA_USER_HEAP_SIZE (1024UL * 1024UL)
#define LUNA_USER_ALIGNMENT 16UL
#define LUNA_USER_MIN_SPLIT 16UL
#define LUNA_STATIC_WORKERS 4
#define LUNA_STATIC_ARGC_MAX 16
#define LUNA_STATIC_ARG_BYTES 1024
#define LUNA_PIPELINE_BENCH_BYTES (1024UL * 1024UL)
#define LUNA_PIPELINE_BENCH_SAMPLES 7U
#define LUNA_BLOCK_BENCH_BYTES (1024UL * 1024UL)
#define LUNA_BLOCK_BENCH_BLOCK 4096UL
#define LUNA_STDIO_IO_CHUNK (64UL * 1024UL)
#define LUNA_STDIO_GETC_BUFFER (16UL * 1024UL)
#define LUNA_BLOCK_BENCH_OPS \
    (LUNA_BLOCK_BENCH_BYTES / LUNA_BLOCK_BENCH_BLOCK)

extern int lbb_main(char **argv);
void *luna_bb_malloc(size_t size);
void luna_bb_free(void *pointer);
off_t luna_bb_lseek(int fd, off_t offset, int whence);

#define DECLARE_WORKER_SLOT(slot) \
    extern int luna_bb_slot##slot##_find_applet_by_name(const char *name); \
    extern int luna_bb_slot##slot##_run_nofork_applet(int applet, char **argv); \
    extern void luna_bb_slot##slot##_lbb_prepare(const char *applet)
DECLARE_WORKER_SLOT(0);
DECLARE_WORKER_SLOT(1);
DECLARE_WORKER_SLOT(2);
DECLARE_WORKER_SLOT(3);

#define DEFINE_WORKER_GETOPT(slot) \
    int luna_bb_slot##slot##_optind; \
    char *luna_bb_slot##slot##_optarg
DEFINE_WORKER_GETOPT(0);
DEFINE_WORKER_GETOPT(1);
DEFINE_WORKER_GETOPT(2);
DEFINE_WORKER_GETOPT(3);

static jmp_buf task_user_exit;
static volatile int task_user_exit_armed;
static volatile int task_user_status;
static volatile int task_user_forbidden_calls;
static int task_user_main_errno;
static unsigned char task_user_heap[LUNA_USER_HEAP_SIZE]
    __attribute__((aligned(16)));

struct task_user_block {
    size_t size;
    struct task_user_block *next;
    int free;
    size_t reserved;
};

static struct task_user_block *task_user_heap_head;
static volatile int task_user_heap_lock;
static void (*task_user_signal_handlers[NSIG])(int);
static volatile unsigned task_static_started;
static volatile unsigned task_static_pipelines;
static volatile unsigned task_static_backgrounds;
static volatile int task_static_getopt_lock;
static volatile size_t task_user_heap_current;
static volatile size_t task_user_heap_peak;
static volatile unsigned task_static_current;
static volatile unsigned task_static_peak;
static volatile int task_stdio_benchmarking;
static volatile unsigned long long task_stdio_read_calls;
static volatile unsigned long long task_stdio_read_bytes;
static volatile unsigned long long task_stdio_write_calls;
static volatile unsigned long long task_stdio_write_bytes;

static void task_stdio_record(volatile unsigned long long *calls,
                              volatile unsigned long long *bytes,
                              ssize_t result)
{
    if (!task_stdio_benchmarking || result < 0) return;
    __atomic_fetch_add(calls, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(bytes, (unsigned long long)result, __ATOMIC_RELAXED);
}

struct task_static_worker {
    volatile int in_use;
    volatile int done;
    volatile int reaped;
    lkl_thread_t tid;
    jmp_buf exit_jump;
    volatile int exit_armed;
    volatile int status;
    int error_number;
    int getopt_locked;
    int fd_map[3];
    int argc;
    char *argv[LUNA_STATIC_ARGC_MAX + 1];
    char argument_data[LUNA_STATIC_ARG_BYTES];
    size_t stdin_buffer_offset;
    size_t stdin_buffer_length;
    unsigned char stdin_buffer[LUNA_STDIO_GETC_BUFFER];
};

static struct task_static_worker task_static_workers[LUNA_STATIC_WORKERS];
static int *task_worker_optind[LUNA_STATIC_WORKERS] = {
    &luna_bb_slot0_optind, &luna_bb_slot1_optind,
    &luna_bb_slot2_optind, &luna_bb_slot3_optind,
};
static char **task_worker_optarg[LUNA_STATIC_WORKERS] = {
    &luna_bb_slot0_optarg, &luna_bb_slot1_optarg,
    &luna_bb_slot2_optarg, &luna_bb_slot3_optarg,
};

typedef int (*task_applet_find_fn)(const char *name);
typedef int (*task_applet_run_fn)(int applet, char **argv);
typedef void (*task_applet_prepare_fn)(const char *applet);
static task_applet_find_fn task_applet_find[LUNA_STATIC_WORKERS] = {
    luna_bb_slot0_find_applet_by_name,
    luna_bb_slot1_find_applet_by_name,
    luna_bb_slot2_find_applet_by_name,
    luna_bb_slot3_find_applet_by_name,
};
static task_applet_run_fn task_applet_run[LUNA_STATIC_WORKERS] = {
    luna_bb_slot0_run_nofork_applet,
    luna_bb_slot1_run_nofork_applet,
    luna_bb_slot2_run_nofork_applet,
    luna_bb_slot3_run_nofork_applet,
};
static task_applet_prepare_fn task_applet_prepare[LUNA_STATIC_WORKERS] = {
    luna_bb_slot0_lbb_prepare,
    luna_bb_slot1_lbb_prepare,
    luna_bb_slot2_lbb_prepare,
    luna_bb_slot3_lbb_prepare,
};

static struct task_static_worker *bb_current_worker(void)
{
    lkl_thread_t tid = lkl_host_ops.thread_self();
    for (int i = 0; i < LUNA_STATIC_WORKERS; i++) {
        struct task_static_worker *worker = &task_static_workers[i];
        if (__atomic_load_n(&worker->in_use, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&worker->tid, __ATOMIC_ACQUIRE) == tid)
            return worker;
    }
    return NULL;
}

static void task_peak_size(volatile size_t *peak, size_t value)
{
    size_t observed = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (value > observed &&
           !__atomic_compare_exchange_n(peak, &observed, value, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
}

static void task_peak_unsigned(volatile unsigned *peak, unsigned value)
{
    unsigned observed = __atomic_load_n(peak, __ATOMIC_RELAXED);
    while (value > observed &&
           !__atomic_compare_exchange_n(peak, &observed, value, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
}

static int *bb_errno_storage(void)
{
    struct task_static_worker *worker = bb_current_worker();
    return worker ? &worker->error_number : &task_user_main_errno;
}

#define task_user_errno (*bb_errno_storage())

static int bb_fd(int fd)
{
    struct task_static_worker *worker = bb_current_worker();
    if (worker && fd >= 0 && fd < 3) return worker->fd_map[fd];
    return fd;
}

static int bb_getopt_begin(struct task_static_worker **worker_out)
{
    struct task_static_worker *worker = bb_current_worker();
    *worker_out = worker;
    if (!worker) return -1;
    int slot = (int)(worker - task_static_workers);
    if (!worker->getopt_locked) {
        while (__atomic_test_and_set(&task_static_getopt_lock,
                                     __ATOMIC_ACQUIRE))
            seL4_Yield();
        worker->getopt_locked = 1;
    }
    optind = *task_worker_optind[slot];
    optarg = *task_worker_optarg[slot];
    return slot;
}

static int bb_getopt_end(struct task_static_worker *worker, int slot,
                         int result)
{
    if (!worker) return result;
    *task_worker_optind[slot] = optind;
    *task_worker_optarg[slot] = optarg;
    if (result == -1 && worker->getopt_locked) {
        worker->getopt_locked = 0;
        __atomic_clear(&task_static_getopt_lock, __ATOMIC_RELEASE);
    }
    return result;
}

int luna_bb_getopt(int argc, char *const argv[], const char *options)
{
    struct task_static_worker *worker;
    int slot = bb_getopt_begin(&worker);
    int result = getopt(argc, argv, options);
    return bb_getopt_end(worker, slot, result);
}

int luna_bb_getopt_long(int argc, char *const argv[], const char *options,
                        const struct option *long_options, int *index)
{
    struct task_static_worker *worker;
    int slot = bb_getopt_begin(&worker);
    int result = getopt_long(argc, argv, options, long_options, index);
    return bb_getopt_end(worker, slot, result);
}

static long bb_result(long result)
{
    if (result < 0) {
        task_user_errno = (int)-result;
        return -1;
    }
    return result;
}

int *luna_bb_errno_location(void)
{
    return &task_user_errno;
}

unsigned int luna_bb_gnu_dev_major(dev_t device)
{
    uint64_t value = (uint64_t)device;
    return (unsigned int)(((value & 0x00000000000fff00ULL) >> 8) |
                          ((value & 0xfffff00000000000ULL) >> 32));
}

unsigned int luna_bb_gnu_dev_minor(dev_t device)
{
    uint64_t value = (uint64_t)device;
    return (unsigned int)((value & 0x00000000000000ffULL) |
                          ((value & 0x00000ffffff00000ULL) >> 12));
}

dev_t luna_bb_gnu_dev_makedev(unsigned int major_number,
                              unsigned int minor_number)
{
    uint64_t value = ((uint64_t)(major_number & 0x00000fffU) << 8) |
                     ((uint64_t)(major_number & 0xfffff000U) << 32) |
                     (uint64_t)(minor_number & 0x000000ffU) |
                     ((uint64_t)(minor_number & 0xffffff00U) << 12);
    return (dev_t)value;
}

static int bb_stat_result(long result, const struct lkl_stat *source,
                          struct stat *target)
{
    if (result < 0) return (int)bb_result(result);
    memset(target, 0, sizeof(*target));
    target->st_dev = source->st_dev;
    target->st_ino = source->st_ino;
    target->st_mode = source->st_mode;
    target->st_nlink = source->st_nlink;
    target->st_uid = source->st_uid;
    target->st_gid = source->st_gid;
    target->st_rdev = source->st_rdev;
    target->st_size = source->st_size;
    target->st_blksize = source->st_blksize;
    target->st_blocks = source->st_blocks;
    target->st_atim.tv_sec = source->lkl_st_atime;
    target->st_atim.tv_nsec = source->st_atime_nsec;
    target->st_mtim.tv_sec = source->lkl_st_mtime;
    target->st_mtim.tv_nsec = source->st_mtime_nsec;
    target->st_ctim.tv_sec = source->lkl_st_ctime;
    target->st_ctim.tv_nsec = source->st_ctime_nsec;
    return 0;
}

int luna_bb_open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    long opened = lkl_sys_open(path, flags, mode);
    if (opened < 0) return (int)bb_result(opened);

    /*
     * A static worker owns a virtual descriptor namespace for stdin,
     * stdout and stderr.  Preserve the POSIX lowest-free-fd rule when an
     * applet closes one of those descriptors before opening a replacement
     * (uniq FILE does this for stdin).
     */
    struct task_static_worker *worker = bb_current_worker();
    if (worker) {
        for (int fd = 0; fd < 3; fd++) {
            if (worker->fd_map[fd] < 0) {
                worker->fd_map[fd] = (int)opened;
                return fd;
            }
        }
    }
    return (int)opened;
}

int luna_bb_close(int fd)
{
    struct task_static_worker *worker = bb_current_worker();
    int actual = bb_fd(fd);
    if (worker && fd >= 0 && fd < 3) {
        worker->fd_map[fd] = -1;
        if (!fd) worker->stdin_buffer_offset = worker->stdin_buffer_length = 0;
    }
    if (actual < 0) {
        task_user_errno = EBADF;
        return -1;
    }
    return (int)bb_result(lkl_sys_close((unsigned int)actual));
}

ssize_t luna_bb_read(int fd, void *buffer, size_t count)
{
    if (!count) return 0;
    int actual = bb_fd(fd);
    if (actual < 0) return (ssize_t)bb_result(-LKL_EBADF);
    size_t buffered = 0;
    struct task_static_worker *worker = bb_current_worker();
    if (worker && !fd && worker->stdin_buffer_offset <
                            worker->stdin_buffer_length) {
        size_t available = worker->stdin_buffer_length -
                           worker->stdin_buffer_offset;
        buffered = count < available ? count : available;
        memcpy(buffer, worker->stdin_buffer + worker->stdin_buffer_offset,
               buffered);
        worker->stdin_buffer_offset += buffered;
        if (buffered == count) return (ssize_t)buffered;
    }
    ssize_t result = (ssize_t)bb_result(lkl_sys_read(
        (unsigned int)actual, (unsigned char *)buffer + buffered,
        count - buffered));
    task_stdio_record(&task_stdio_read_calls, &task_stdio_read_bytes, result);
    return result < 0 ? (buffered ? (ssize_t)buffered : result) :
                        (ssize_t)buffered + result;
}

ssize_t luna_bb_write(int fd, const void *buffer, size_t count)
{
    int actual = bb_fd(fd);
    if (actual < 0) return (ssize_t)bb_result(-LKL_EBADF);
    ssize_t result = (ssize_t)bb_result(lkl_sys_write((unsigned int)actual,
                                                      (void *)buffer, count));
    task_stdio_record(&task_stdio_write_calls, &task_stdio_write_bytes,
                      result);
    return result;
}

static int bb_write_all(int fd, const void *buffer, size_t count)
{
    const unsigned char *cursor = buffer;
    size_t written = 0;
    while (written < count) {
        ssize_t result = luna_bb_write(fd, cursor + written,
                                       count - written);
        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int bb_vformat_fd(int fd, const char *format, va_list arguments)
{
    char stack_buffer[512];
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(stack_buffer, sizeof(stack_buffer), format, copy);
    va_end(copy);
    if (length < 0) return -1;

    char *buffer = stack_buffer;
    if ((size_t)length >= sizeof(stack_buffer)) {
        buffer = luna_bb_malloc((size_t)length + 1U);
        if (!buffer) return -1;
        va_copy(copy, arguments);
        int formatted = vsnprintf(buffer, (size_t)length + 1U,
                                  format, copy);
        va_end(copy);
        if (formatted != length) {
            luna_bb_free(buffer);
            task_user_errno = EIO;
            return -1;
        }
    }
    int result = bb_write_all(fd, buffer, (size_t)length) ? -1 : length;
    if (buffer != stack_buffer) luna_bb_free(buffer);
    return result;
}

#define LUNA_BB_FILE_MAGIC 0x4c424246U
struct luna_bb_file {
    uint32_t magic;
    int fd;
    int owned;
    int error;
    int eof;
    int ungot;
};

#define LUNA_BB_DIR_MAGIC 0x4c424244U
struct luna_bb_dir {
    uint32_t magic;
    struct lkl_dir *directory;
    struct dirent entry;
};

static struct luna_bb_file *bb_file_object(FILE *stream)
{
    struct luna_bb_file *file = (void *)stream;
    return file && stream != stdin && stream != stdout && stream != stderr &&
           file->magic == LUNA_BB_FILE_MAGIC ? file : NULL;
}

static ssize_t bb_stream_read(struct luna_bb_file *file, int fd,
                              void *buffer, size_t count)
{
    if (!file) return luna_bb_read(fd, buffer, count);
    long raw = lkl_sys_read((unsigned int)file->fd, buffer, count);
    if (raw < 0) return (ssize_t)bb_result(raw);
    task_stdio_record(&task_stdio_read_calls, &task_stdio_read_bytes,
                      (ssize_t)raw);
    return (ssize_t)raw;
}

static int bb_mode_flags(const char *mode)
{
    if (!mode || !mode[0]) return -1;
    int flags;
    switch (mode[0]) {
    case 'r': flags = O_RDONLY; break;
    case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    if (strchr(mode, '+')) {
        flags &= ~(O_RDONLY | O_WRONLY);
        flags |= O_RDWR;
    }
    return flags;
}

FILE *luna_bb_fdopen(int fd, const char *mode)
{
    if (bb_mode_flags(mode) < 0) {
        task_user_errno = EINVAL;
        return NULL;
    }
    struct luna_bb_file *file = luna_bb_malloc(sizeof(*file));
    if (!file) return NULL;
    file->magic = LUNA_BB_FILE_MAGIC;
    file->fd = bb_fd(fd);
    file->owned = 1;
    file->error = 0;
    file->eof = 0;
    file->ungot = -1;
    return (FILE *)file;
}

FILE *luna_bb_fopen(const char *path, const char *mode)
{
    int flags = bb_mode_flags(mode);
    if (flags < 0) {
        task_user_errno = EINVAL;
        return NULL;
    }
    int fd = luna_bb_open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE *stream = luna_bb_fdopen(fd, mode);
    if (!stream) luna_bb_close(fd);
    return stream;
}

int luna_bb_fileno(FILE *stream)
{
    if (stream == stdin) return 0;
    if (stream == stdout) return 1;
    if (stream == stderr) return 2;
    struct luna_bb_file *file = bb_file_object(stream);
    if (file) return file->fd;
    task_user_errno = EBADF;
    return -1;
}

int luna_bb_fileno_unlocked(FILE *stream)
{
    return luna_bb_fileno(stream);
}

int luna_bb_fclose(FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    if (!file) {
        if (stream == stdin || stream == stdout || stream == stderr) return 0;
        task_user_errno = EBADF;
        return EOF;
    }
    int result = 0;
    if (file->owned && file->fd >= 0)
        result = (int)bb_result(lkl_sys_close((unsigned int)file->fd));
    file->magic = 0;
    luna_bb_free(file);
    return result ? EOF : 0;
}

int luna_bb_getc(FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    int fd = luna_bb_fileno(stream);
    if (fd < 0) return EOF;
    if (file && file->ungot >= 0) {
        int value = file->ungot;
        file->ungot = -1;
        file->eof = 0;
        return value;
    }
    if (!file && stream == stdin) {
        struct task_static_worker *worker = bb_current_worker();
        if (worker) {
            if (worker->stdin_buffer_offset == worker->stdin_buffer_length) {
                ssize_t filled = luna_bb_read(
                    fd, worker->stdin_buffer, sizeof(worker->stdin_buffer));
                if (filled <= 0) return EOF;
                worker->stdin_buffer_offset = 0;
                worker->stdin_buffer_length = (size_t)filled;
            }
            return worker->stdin_buffer[worker->stdin_buffer_offset++];
        }
    }
    unsigned char value;
    ssize_t result = bb_stream_read(file, fd, &value, 1);
    if (result == 1) return value;
    if (file) {
        if (result == 0) file->eof = 1;
        else file->error = 1;
    }
    return EOF;
}

int luna_bb_getc_unlocked(FILE *stream)
{
    return luna_bb_getc(stream);
}

int luna_bb_fgetc(FILE *stream)
{
    return luna_bb_getc(stream);
}

int luna_bb_ungetc(int character, FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    if (!file || character == EOF || file->ungot >= 0) return EOF;
    file->ungot = (unsigned char)character;
    file->eof = 0;
    return file->ungot;
}

size_t luna_bb_fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) {
        task_user_errno = EOVERFLOW;
        struct luna_bb_file *overflow_file = bb_file_object(stream);
        if (overflow_file) overflow_file->error = 1;
        return 0;
    }
    struct luna_bb_file *file = bb_file_object(stream);
    int fd = luna_bb_fileno(stream);
    if (fd < 0) return 0;
    unsigned char *cursor = buffer;
    size_t bytes = size * count;
    size_t read_count = 0;
    if (file && file->ungot >= 0) {
        cursor[read_count++] = (unsigned char)file->ungot;
        file->ungot = -1;
        file->eof = 0;
    }
    while (read_count < bytes) {
        size_t remaining = bytes - read_count;
        size_t chunk = remaining < LUNA_STDIO_IO_CHUNK ? remaining :
                       LUNA_STDIO_IO_CHUNK;
        ssize_t result = bb_stream_read(file, fd, cursor + read_count, chunk);
        if (result <= 0) {
            if (file) {
                if (!result) file->eof = 1;
                else file->error = 1;
            }
            break;
        }
        read_count += (size_t)result;
        if ((size_t)result < chunk) break;
    }
    return read_count / size;
}

size_t luna_bb_fwrite(const void *buffer, size_t size, size_t count,
                      FILE *stream)
{
    if (!size || !count) return 0;
    if (count > SIZE_MAX / size) {
        task_user_errno = EOVERFLOW;
        struct luna_bb_file *overflow_file = bb_file_object(stream);
        if (overflow_file) overflow_file->error = 1;
        return 0;
    }
    int fd = luna_bb_fileno(stream);
    if (fd < 0) return 0;
    size_t bytes = size * count;
    const unsigned char *cursor = buffer;
    size_t written = 0;
    while (written < bytes) {
        size_t remaining = bytes - written;
        size_t chunk = remaining < LUNA_STDIO_IO_CHUNK ? remaining :
                       LUNA_STDIO_IO_CHUNK;
        ssize_t result = luna_bb_write(fd, cursor + written, chunk);
        if (result <= 0) break;
        written += (size_t)result;
    }
    struct luna_bb_file *file = bb_file_object(stream);
    if (file && written != bytes) file->error = 1;
    return written / size;
}

char *luna_bb_fgets(char *buffer, int size, FILE *stream)
{
    if (!buffer || size <= 0) return NULL;
    int used = 0;
    while (used + 1 < size) {
        int value = luna_bb_getc(stream);
        if (value == EOF) break;
        buffer[used++] = (char)value;
        if (value == '\n') break;
    }
    if (!used) return NULL;
    buffer[used] = '\0';
    return buffer;
}

int luna_bb_feof(FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    return file ? file->eof : 0;
}

int luna_bb_fseek(FILE *stream, long offset, int whence)
{
    int fd = luna_bb_fileno(stream);
    if (fd < 0 || luna_bb_lseek(fd, offset, whence) < 0) return -1;
    struct luna_bb_file *file = bb_file_object(stream);
    if (file) {
        file->eof = 0;
        file->ungot = -1;
    }
    return 0;
}

long luna_bb_ftell(FILE *stream)
{
    int fd = luna_bb_fileno(stream);
    return fd < 0 ? -1 : (long)luna_bb_lseek(fd, 0, SEEK_CUR);
}

void luna_bb_rewind(FILE *stream)
{
    luna_bb_fseek(stream, 0, SEEK_SET);
    struct luna_bb_file *file = bb_file_object(stream);
    if (file) file->error = 0;
}

int luna_bb_vprintf(const char *format, va_list arguments)
{
    return bb_vformat_fd(1, format, arguments);
}

int luna_bb_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = bb_vformat_fd(1, format, arguments);
    va_end(arguments);
    return result;
}

int luna_bb_dprintf(int fd, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = bb_vformat_fd(fd, format, arguments);
    va_end(arguments);
    return result;
}

static int bb_file_fd(FILE *stream)
{
    if (stream == stdin) return 0;
    if (stream == stdout) return 1;
    if (stream == stderr) return 2;
    struct luna_bb_file *file = bb_file_object(stream);
    if (file) return file->fd;
    task_user_errno = ENOSYS;
    return -1;
}

int luna_bb_vfprintf(FILE *stream, const char *format, va_list arguments)
{
    int fd = bb_file_fd(stream);
    return fd < 0 ? -1 : bb_vformat_fd(fd, format, arguments);
}

int luna_bb_fprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = luna_bb_vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int luna_bb_putchar(int character)
{
    unsigned char value = (unsigned char)character;
    return bb_write_all(1, &value, 1) ? EOF : value;
}

int luna_bb_putchar_unlocked(int character)
{
    return luna_bb_putchar(character);
}

int luna_bb_puts(const char *string)
{
    size_t length = strlen(string);
    if (bb_write_all(1, string, length) || bb_write_all(1, "\n", 1))
        return EOF;
    return 0;
}

int luna_bb_fputs(const char *string, FILE *stream)
{
    int fd = bb_file_fd(stream);
    if (fd < 0 || bb_write_all(fd, string, strlen(string))) return EOF;
    return 0;
}

int luna_bb_fputs_unlocked(const char *string, FILE *stream)
{
    return luna_bb_fputs(string, stream);
}

int luna_bb_putc(int character, FILE *stream)
{
    int fd = bb_file_fd(stream);
    if (fd < 0) return EOF;
    unsigned char value = (unsigned char)character;
    return bb_write_all(fd, &value, 1) ? EOF : value;
}

int luna_bb_putc_unlocked(int character, FILE *stream)
{
    return luna_bb_putc(character, stream);
}

int luna_bb_fflush(FILE *stream)
{
    if (!stream || stream == stdin || stream == stdout || stream == stderr ||
        bb_file_object(stream)) return 0;
    task_user_errno = ENOSYS;
    return EOF;
}

int luna_bb_ferror(FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    return file ? file->error : bb_file_fd(stream) < 0;
}

int luna_bb_ferror_unlocked(FILE *stream)
{
    return luna_bb_ferror(stream);
}

int luna_bb_getchar(void)
{
    return luna_bb_getc(stdin);
}

int luna_bb_getchar_unlocked(void)
{
    return luna_bb_getc(stdin);
}

void luna_bb_setbuf(FILE *stream, char *buffer)
{
    (void)stream;
    (void)buffer;
}

int luna_bb_setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    (void)buffer;
    (void)mode;
    (void)size;
    if (stream == stdin || stream == stdout || stream == stderr ||
        bb_file_object(stream)) return 0;
    task_user_errno = EBADF;
    return -1;
}

void luna_bb_clearerr(FILE *stream)
{
    struct luna_bb_file *file = bb_file_object(stream);
    if (file) {
        file->error = 0;
        file->eof = 0;
    }
}

off_t luna_bb_lseek(int fd, off_t offset, int whence)
{
    struct task_static_worker *worker = bb_current_worker();
    if (worker && !fd && worker->stdin_buffer_offset <
                            worker->stdin_buffer_length) {
        if (whence == SEEK_CUR)
            offset -= (off_t)(worker->stdin_buffer_length -
                              worker->stdin_buffer_offset);
        worker->stdin_buffer_offset = worker->stdin_buffer_length = 0;
    }
    return (off_t)bb_result(lkl_sys_lseek((unsigned int)bb_fd(fd), offset,
                                          whence));
}

int luna_bb_dup(int fd)
{
    return (int)bb_result(lkl_sys_dup((unsigned int)bb_fd(fd)));
}

int luna_bb_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd) return oldfd;
    struct task_static_worker *worker = bb_current_worker();
    if (worker && newfd >= 0 && newfd < 3) {
        long duplicate = lkl_sys_dup((unsigned int)bb_fd(oldfd));
        if (duplicate < 0) return (int)bb_result(duplicate);
        if (worker->fd_map[newfd] >= 0)
            lkl_sys_close((unsigned int)worker->fd_map[newfd]);
        worker->fd_map[newfd] = (int)duplicate;
        if (!newfd)
            worker->stdin_buffer_offset = worker->stdin_buffer_length = 0;
        return newfd;
    }
    return (int)bb_result(lkl_sys_dup3((unsigned int)bb_fd(oldfd),
                                      (unsigned int)newfd, 0));
}

int luna_bb_fcntl(int fd, int command, ...)
{
    unsigned long argument = 0;
    if (command != F_GETFD && command != F_GETFL) {
        va_list ap;
        va_start(ap, command);
        argument = va_arg(ap, unsigned long);
        va_end(ap);
    }
    return (int)bb_result(lkl_sys_fcntl((unsigned int)bb_fd(fd), command,
                                        argument));
}

int luna_bb_fstat(int fd, struct stat *target)
{
    struct lkl_stat source;
    long result = lkl_sys_fstat((unsigned int)bb_fd(fd), &source);
    return bb_stat_result(result, &source, target);
}

int luna_bb_stat(const char *path, struct stat *target)
{
    struct lkl_stat source;
    long result = lkl_sys_stat(path, &source);
    return bb_stat_result(result, &source, target);
}

int luna_bb_lstat(const char *path, struct stat *target)
{
    struct lkl_stat source;
    long result = lkl_sys_lstat(path, &source);
    return bb_stat_result(result, &source, target);
}

int luna_bb_access(const char *path, int mode)
{
    return (int)bb_result(lkl_sys_access(path, mode));
}

int luna_bb_chown(const char *path, uid_t user, gid_t group)
{
    return (int)bb_result(lkl_sys_chown(path, user, group));
}

int luna_bb_lchown(const char *path, uid_t user, gid_t group)
{
    return (int)bb_result(lkl_sys_fchownat(LKL_AT_FDCWD, path, user, group,
                                           LKL_AT_SYMLINK_NOFOLLOW));
}

int luna_bb_mknod(const char *path, mode_t mode, dev_t device)
{
    return (int)bb_result(lkl_sys_mknod(path, mode, device));
}

int luna_bb_utimes(const char *path, const struct timeval times[2])
{
    struct __lkl__kernel_timespec converted[2];
    if (times) {
        for (int i = 0; i < 2; i++) {
            converted[i].tv_sec = times[i].tv_sec;
            converted[i].tv_nsec = times[i].tv_usec * 1000L;
        }
    }
    return (int)bb_result(lkl_sys_utimensat(LKL_AT_FDCWD, path,
                                            times ? converted : NULL, 0));
}

int luna_bb_gettimeofday(struct timeval *time_value, void *timezone)
{
    (void)timezone;
    if (!time_value) {
        task_user_errno = EINVAL;
        return -1;
    }
    unsigned long long now = luna_lkl_task_time();
    time_value->tv_sec = (time_t)(now / 1000000000ULL);
    time_value->tv_usec = (suseconds_t)((now % 1000000000ULL) / 1000ULL);
    return 0;
}

DIR *luna_bb_opendir(const char *path)
{
    int error = 0;
    struct lkl_dir *directory = lkl_opendir(path, &error);
    if (!directory) {
        task_user_errno = error < 0 ? -error : error;
        return NULL;
    }
    struct luna_bb_dir *stream = luna_bb_malloc(sizeof(*stream));
    if (!stream) {
        lkl_closedir(directory);
        return NULL;
    }
    memset(stream, 0, sizeof(*stream));
    stream->magic = LUNA_BB_DIR_MAGIC;
    stream->directory = directory;
    return (DIR *)stream;
}

DIR *luna_bb_fdopendir(int fd)
{
    int actual = bb_fd(fd);
    int error = 0;
    struct lkl_dir *directory = lkl_fdopendir(actual, &error);
    if (!directory) {
        task_user_errno = error < 0 ? -error : error;
        return NULL;
    }
    struct luna_bb_dir *stream = luna_bb_malloc(sizeof(*stream));
    if (!stream) {
        lkl_closedir(directory);
        return NULL;
    }
    memset(stream, 0, sizeof(*stream));
    stream->magic = LUNA_BB_DIR_MAGIC;
    stream->directory = directory;
    return (DIR *)stream;
}

struct dirent *luna_bb_readdir(DIR *directory)
{
    struct luna_bb_dir *stream = (void *)directory;
    if (!stream || stream->magic != LUNA_BB_DIR_MAGIC) {
        task_user_errno = EBADF;
        return NULL;
    }
    struct lkl_linux_dirent64 *entry = lkl_readdir(stream->directory);
    if (!entry) {
        int error = lkl_errdir(stream->directory);
        if (error < 0) task_user_errno = -error;
        return NULL;
    }
    size_t length = strnlen(entry->d_name, sizeof(stream->entry.d_name) - 1U);
    stream->entry.d_ino = (ino_t)entry->d_ino;
    stream->entry.d_off = (off_t)entry->d_off;
    stream->entry.d_reclen = sizeof(stream->entry);
    stream->entry.d_type = entry->d_type;
    memcpy(stream->entry.d_name, entry->d_name, length);
    stream->entry.d_name[length] = '\0';
    return &stream->entry;
}

int luna_bb_closedir(DIR *directory)
{
    struct luna_bb_dir *stream = (void *)directory;
    if (!stream || stream->magic != LUNA_BB_DIR_MAGIC) {
        task_user_errno = EBADF;
        return -1;
    }
    int result = lkl_closedir(stream->directory);
    stream->magic = 0;
    luna_bb_free(stream);
    return (int)bb_result(result);
}

void luna_bb_rewinddir(DIR *directory)
{
    struct luna_bb_dir *stream = (void *)directory;
    if (stream && stream->magic == LUNA_BB_DIR_MAGIC)
        lkl_rewinddir(stream->directory);
}

int luna_bb_dirfd(DIR *directory)
{
    struct luna_bb_dir *stream = (void *)directory;
    if (!stream || stream->magic != LUNA_BB_DIR_MAGIC) {
        task_user_errno = EBADF;
        return -1;
    }
    return lkl_dirfd(stream->directory);
}

int luna_bb_chdir(const char *path)
{
    return (int)bb_result(lkl_sys_chdir(path));
}

int luna_bb_fchdir(int fd)
{
    return (int)bb_result(lkl_sys_fchdir((unsigned int)bb_fd(fd)));
}

int luna_bb_chroot(const char *path)
{
    return (int)bb_result(lkl_sys_chroot(path));
}

char *luna_bb_getcwd(char *buffer, size_t size)
{
    if (bb_result(lkl_sys_getcwd(buffer, size)) < 0) return NULL;
    return buffer;
}

int luna_bb_mkdir(const char *path, mode_t mode)
{
    return (int)bb_result(lkl_sys_mkdir(path, mode));
}

int luna_bb_rmdir(const char *path)
{
    return (int)bb_result(lkl_sys_rmdir(path));
}

int luna_bb_chmod(const char *path, mode_t mode)
{
    return (int)bb_result(lkl_sys_chmod(path, mode));
}

int luna_bb_ftruncate(int fd, off_t length)
{
    return (int)bb_result(lkl_sys_ftruncate((unsigned int)bb_fd(fd), length));
}

int luna_bb_uname(struct utsname *name)
{
    struct lkl_utsname_wire {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    } source;
    long parameters[6] = { (long)&source };
    long result = lkl_syscall(__lkl__NR_uname, parameters);
    if (result < 0) return (int)bb_result(result);
    memset(name, 0, sizeof(*name));
    memcpy(name->sysname, source.sysname, sizeof(name->sysname));
    memcpy(name->nodename, source.nodename, sizeof(name->nodename));
    memcpy(name->release, source.release, sizeof(name->release));
    memcpy(name->version, source.version, sizeof(name->version));
    memcpy(name->machine, source.machine, sizeof(name->machine));
    return 0;
}

static void bb_lkl_times(const struct timespec source[2],
                         struct __lkl__kernel_timespec target[2])
{
    if (!source) return;
    for (int i = 0; i < 2; i++) {
        target[i].tv_sec = source[i].tv_sec;
        target[i].tv_nsec = source[i].tv_nsec;
    }
}

int luna_bb_utimensat(int dirfd, const char *path,
                      const struct timespec times[2], int flags)
{
    struct __lkl__kernel_timespec converted[2];
    bb_lkl_times(times, converted);
    int lkl_dirfd = dirfd == AT_FDCWD ? LKL_AT_FDCWD : dirfd;
    return (int)bb_result(lkl_sys_utimensat(lkl_dirfd, path,
                                            times ? converted : NULL,
                                            flags));
}

int luna_bb_futimens(int fd, const struct timespec times[2])
{
    struct __lkl__kernel_timespec converted[2];
    bb_lkl_times(times, converted);
    return (int)bb_result(lkl_sys_utimensat(bb_fd(fd), NULL,
                                            times ? converted : NULL, 0));
}

ssize_t luna_bb_readlink(const char *path, char *buffer, size_t size)
{
    return (ssize_t)bb_result(lkl_sys_readlink(path, buffer, size));
}

int luna_bb_symlink(const char *target, const char *path)
{
    return (int)bb_result(lkl_sys_symlink(target, path));
}

int luna_bb_link(const char *target, const char *path)
{
    return (int)bb_result(lkl_sys_link(target, path));
}

static int bb_realpath_pop(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        *slash = '\0';
    }
    return 0;
}

char *luna_bb_realpath(const char *path, char *resolved)
{
    char pending[PATH_MAX];
    char output[PATH_MAX];
    char link_target[PATH_MAX];
    if (!path || !*path) {
        task_user_errno = ENOENT;
        return NULL;
    }
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(pending)) goto too_long;
        strcpy(pending, path);
    } else {
        if (!luna_bb_getcwd(pending, sizeof(pending))) return NULL;
        size_t cwd_length = strlen(pending);
        size_t path_length = strlen(path);
        if (cwd_length + 1U + path_length >= sizeof(pending)) goto too_long;
        if (cwd_length > 1U) pending[cwd_length++] = '/';
        strcpy(pending + cwd_length, path);
    }
    strcpy(output, "/");
    unsigned links = 0;
    char *cursor = pending;
    while (*cursor) {
        while (*cursor == '/') cursor++;
        if (!*cursor) break;
        char *end = cursor;
        while (*end && *end != '/') end++;
        char saved = *end;
        *end = '\0';
        if (!strcmp(cursor, ".")) {
            *end = saved;
            cursor = end;
            continue;
        }
        if (!strcmp(cursor, "..")) {
            bb_realpath_pop(output);
            *end = saved;
            cursor = end;
            continue;
        }
        size_t output_length = strlen(output);
        size_t component_length = strlen(cursor);
        if (output_length + (output_length > 1U) + component_length >=
            sizeof(output)) goto too_long;
        if (output_length > 1U) output[output_length++] = '/';
        strcpy(output + output_length, cursor);
        struct stat status;
        if (luna_bb_lstat(output, &status)) return NULL;
        if (S_ISLNK(status.st_mode)) {
            if (++links > 40U) {
                task_user_errno = ELOOP;
                return NULL;
            }
            ssize_t length = luna_bb_readlink(output, link_target,
                                              sizeof(link_target) - 1U);
            if (length < 0) return NULL;
            link_target[length] = '\0';
            bb_realpath_pop(output);
            const char *remainder = saved ? end + 1 : end;
            size_t target_length = (size_t)length;
            size_t remainder_length = strlen(remainder);
            char replacement[PATH_MAX];
            if (target_length + (remainder_length != 0) + remainder_length >=
                sizeof(replacement)) goto too_long;
            strcpy(replacement, link_target);
            if (remainder_length) {
                replacement[target_length++] = '/';
                strcpy(replacement + target_length, remainder);
            }
            if (link_target[0] == '/') strcpy(output, "/");
            strcpy(pending, replacement);
            cursor = pending;
            continue;
        }
        *end = saved;
        cursor = end;
    }
    size_t length = strlen(output) + 1U;
    char *result = resolved ? resolved : luna_bb_malloc(length);
    if (!result) return NULL;
    memcpy(result, output, length);
    return result;

too_long:
    task_user_errno = ENAMETOOLONG;
    return NULL;
}

int luna_bb_isatty(int fd)
{
    struct termios attributes;
    long result = lkl_sys_ioctl((unsigned int)bb_fd(fd), TCGETS,
                                (unsigned long)&attributes);
    if (result < 0) {
        task_user_errno = (int)-result;
        return 0;
    }
    return 1;
}

int luna_bb_ioctl(int fd, unsigned long request, ...)
{
    unsigned long argument;
    va_list ap;
    va_start(ap, request);
    argument = va_arg(ap, unsigned long);
    va_end(ap);
    return (int)bb_result(lkl_sys_ioctl((unsigned int)bb_fd(fd),
                                        request, argument));
}

int luna_bb_tcgetattr(int fd, struct termios *termios)
{
    return (int)bb_result(lkl_sys_ioctl((unsigned int)bb_fd(fd), TCGETS,
                                        (unsigned long)termios));
}

int luna_bb_tcsetattr(int fd, int action, const struct termios *termios)
{
    unsigned long request;
    switch (action) {
    case TCSANOW: request = TCSETS; break;
    case TCSADRAIN: request = TCSETSW; break;
    case TCSAFLUSH: request = TCSETSF; break;
    default:
        task_user_errno = EINVAL;
        return -1;
    }
    return (int)bb_result(lkl_sys_ioctl((unsigned int)bb_fd(fd), request,
                                        (unsigned long)termios));
}

unsigned int luna_bb_sleep(unsigned int seconds)
{
    struct __lkl__kernel_timespec request = {
        .tv_sec = seconds,
        .tv_nsec = 0,
    };
    struct __lkl__kernel_timespec remaining = { 0 };
    long result = lkl_sys_nanosleep(&request, &remaining);
    if (result >= 0) return 0;
    task_user_errno = (int)-result;
    if (remaining.tv_sec < 0) return seconds;
    unsigned long long unslept = (unsigned long long)remaining.tv_sec +
                                 (remaining.tv_nsec != 0);
    return unslept > UINT32_MAX ? UINT32_MAX : (unsigned int)unslept;
}

void luna_bb_sync(void)
{
    lkl_sys_sync();
}

int luna_bb_fsync(int fd)
{
    return (int)bb_result(lkl_sys_fsync((unsigned int)bb_fd(fd)));
}

int luna_bb_fdatasync(int fd)
{
    return (int)bb_result(lkl_sys_fdatasync((unsigned int)bb_fd(fd)));
}

mode_t luna_bb_umask(mode_t mask)
{
    return (mode_t)bb_result(lkl_sys_umask(mask));
}

int luna_bb_unlink(const char *path)
{
    return (int)bb_result(lkl_sys_unlink(path));
}

int luna_bb_rename(const char *old_path, const char *new_path)
{
    return (int)bb_result(lkl_sys_rename(old_path, new_path));
}

pid_t luna_bb_getpid(void)
{
    struct task_static_worker *worker = bb_current_worker();
    return worker ? (pid_t)worker->tid : 2;
}
pid_t luna_bb_getppid(void) { return bb_current_worker() ? 2 : 1; }
uid_t luna_bb_geteuid(void) { return 0; }
gid_t luna_bb_getegid(void) { return 0; }
int luna_bb_mallopt(int parameter, int value)
{
    (void)parameter;
    (void)value;
    return 1;
}

static void task_user_heap_acquire(void)
{
    while (__atomic_test_and_set(&task_user_heap_lock, __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void task_user_heap_release(void)
{
    __atomic_clear(&task_user_heap_lock, __ATOMIC_RELEASE);
}

void *luna_bb_malloc(size_t size)
{
    if (!size || size > SIZE_MAX - (LUNA_USER_ALIGNMENT - 1U)) {
        task_user_errno = ENOMEM;
        return NULL;
    }
    task_user_heap_acquire();
    size_t aligned = (size + LUNA_USER_ALIGNMENT - 1U) &
                     ~(LUNA_USER_ALIGNMENT - 1U);
    for (struct task_user_block *block = task_user_heap_head;
         block; block = block->next) {
        if (!block->free || block->size < aligned) continue;
        size_t remaining = block->size - aligned;
        if (remaining >= sizeof(struct task_user_block) +
                         LUNA_USER_MIN_SPLIT) {
            struct task_user_block *split =
                (void *)((unsigned char *)(block + 1) + aligned);
            split->size = remaining - sizeof(*split);
            split->next = block->next;
            split->free = 1;
            block->size = aligned;
            block->next = split;
        }
        block->free = 0;
        size_t current = __atomic_add_fetch(&task_user_heap_current,
                                            block->size,
                                            __ATOMIC_RELAXED);
        task_peak_size(&task_user_heap_peak, current);
        task_user_heap_release();
        return block + 1;
    }
    task_user_heap_release();
    task_user_errno = ENOMEM;
    return NULL;
}

void luna_bb_free(void *pointer)
{
    if (!pointer) return;
    task_user_heap_acquire();
    struct task_user_block *previous = NULL;
    struct task_user_block *block = task_user_heap_head;
    while (block && block + 1 != pointer) {
        previous = block;
        block = block->next;
    }
    if (!block || block->free) {
        task_user_heap_release();
        return;
    }
    size_t released = block->size;
    block->free = 1;
    if (block->next && block->next->free) {
        block->size += sizeof(*block) + block->next->size;
        block->next = block->next->next;
    }
    if (previous && previous->free) {
        previous->size += sizeof(*previous) + block->size;
        previous->next = block->next;
    }
    __atomic_sub_fetch(&task_user_heap_current, released, __ATOMIC_RELAXED);
    task_user_heap_release();
}

void *luna_bb_realloc(void *pointer, size_t size)
{
    if (!pointer) return luna_bb_malloc(size);
    if (!size) {
        luna_bb_free(pointer);
        return NULL;
    }
    task_user_heap_acquire();
    struct task_user_block *block = task_user_heap_head;
    while (block && block + 1 != pointer) block = block->next;
    if (!block || block->free) {
        task_user_heap_release();
        task_user_errno = EINVAL;
        return NULL;
    }
    if (size <= block->size) {
        task_user_heap_release();
        return pointer;
    }
    size_t old_size = block->size;
    task_user_heap_release();
    void *replacement = luna_bb_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, old_size);
    luna_bb_free(pointer);
    return replacement;
}

char *luna_bb_strdup(const char *source)
{
    size_t length = strlen(source) + 1;
    char *copy = luna_bb_malloc(length);
    if (copy) memcpy(copy, source, length);
    return copy;
}

char *luna_bb_strndup(const char *source, size_t maximum)
{
    size_t length = strnlen(source, maximum);
    char *copy = luna_bb_malloc(length + 1);
    if (copy) {
        memcpy(copy, source, length);
        copy[length] = '\0';
    }
    return copy;
}

int luna_bb_raise(int signal_number)
{
    if (signal_number > 0 && signal_number < NSIG) {
        void (*handler)(int) = task_user_signal_handlers[signal_number];
        if (handler && handler != SIG_IGN && handler != SIG_DFL)
            handler(signal_number);
    }
    return 0;
}

void (*luna_bb_signal(int signal_number, void (*handler)(int)))(int)
{
    if (signal_number <= 0 || signal_number >= NSIG) {
        task_user_errno = EINVAL;
        return SIG_ERR;
    }
    void (*old_handler)(int) = task_user_signal_handlers[signal_number];
    task_user_signal_handlers[signal_number] = handler;
    return old_handler ? old_handler : SIG_DFL;
}

int luna_bb_sigaction(int signal_number, const struct sigaction *action,
                      struct sigaction *old_action)
{
    if (signal_number <= 0 || signal_number >= NSIG) {
        task_user_errno = EINVAL;
        return -1;
    }
    if (old_action) {
        memset(old_action, 0, sizeof(*old_action));
        old_action->sa_handler = task_user_signal_handlers[signal_number] ?:
                                 SIG_DFL;
    }
    if (action) task_user_signal_handlers[signal_number] = action->sa_handler;
    return 0;
}

int luna_bb_sigaddset(sigset_t *set, int signal_number)
{
    return sigaddset(set, signal_number);
}

int luna_bb_sigemptyset(sigset_t *set)
{
    return sigemptyset(set);
}

int luna_bb_sigfillset(sigset_t *set)
{
    return sigfillset(set);
}

int luna_bb_sigprocmask(int how, const sigset_t *set, sigset_t *old_set)
{
    (void)how;
    (void)set;
    if (old_set) memset(old_set, 0, sizeof(*old_set));
    return 0;
}

int luna_bb_sigsuspend(const sigset_t *mask)
{
    (void)mask;
    task_user_errno = EINTR;
    return -1;
}

int luna_bb_prctl(int option, ...)
{
    (void)option;
    return 0;
}

static int bb_forbidden(void)
{
    __atomic_add_fetch(&task_user_forbidden_calls, 1, __ATOMIC_RELAXED);
    task_user_errno = ENOSYS;
    return -1;
}

pid_t luna_bb_fork(void) { return (pid_t)bb_forbidden(); }
pid_t luna_bb_vfork(void) { return (pid_t)bb_forbidden(); }
int luna_bb_execve(const char *path, char *const argv[], char *const envp[])
{
    (void)path; (void)argv; (void)envp;
    return bb_forbidden();
}
int luna_bb_execvp(const char *file, char *const argv[])
{
    (void)file; (void)argv;
    return bb_forbidden();
}

static int task_static_command_allowed(const char *name)
{
    static const char *const commands[] = {
        "basename", "cat", "cmp", "cp", "cut", "dd", "dirname", "echo",
        "false", "find", "grep", "head", "ln", "ls", "mkdir", "mv",
        "printenv", "printf", "readlink", "realpath", "rmdir", "sleep",
        "sort", "tee", "touch", "true", "truncate", "uname", "uniq", "unlink",
        "wc",
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (!strcmp(name, commands[i])) return 1;
    return 0;
}

static struct task_static_worker *task_static_allocate(void)
{
    for (int i = 0; i < LUNA_STATIC_WORKERS; i++) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&task_static_workers[i].in_use,
                                        &expected, 1, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            struct task_static_worker *worker = &task_static_workers[i];
            worker->done = 0;
            worker->reaped = 0;
            worker->tid = 0;
            worker->exit_armed = 0;
            worker->status = 255;
            worker->error_number = 0;
            worker->getopt_locked = 0;
            worker->stdin_buffer_offset = 0;
            worker->stdin_buffer_length = 0;
            for (int fd = 0; fd < 3; fd++) worker->fd_map[fd] = -1;
            unsigned current = __atomic_add_fetch(&task_static_current, 1U,
                                                  __ATOMIC_RELAXED);
            task_peak_unsigned(&task_static_peak, current);
            return worker;
        }
    }
    task_user_errno = EAGAIN;
    return NULL;
}

static void task_static_release(struct task_static_worker *worker)
{
    for (int fd = 0; fd < 3; fd++) {
        if (worker->fd_map[fd] >= 0) {
            lkl_sys_close((unsigned int)worker->fd_map[fd]);
            worker->fd_map[fd] = -1;
        }
    }
    __atomic_sub_fetch(&task_static_current, 1U, __ATOMIC_RELAXED);
    __atomic_store_n(&worker->in_use, 0, __ATOMIC_RELEASE);
}

static int task_static_copy_argv(struct task_static_worker *worker,
                                 char **argv)
{
    size_t used = 0;
    int argc = 0;
    while (argv[argc]) {
        if (argc >= LUNA_STATIC_ARGC_MAX) return -LKL_E2BIG;
        size_t length = strlen(argv[argc]) + 1U;
        if (used + length > sizeof(worker->argument_data)) return -LKL_E2BIG;
        worker->argv[argc] = worker->argument_data + used;
        memcpy(worker->argv[argc], argv[argc], length);
        used += length;
        argc++;
    }
    worker->argv[argc] = NULL;
    worker->argc = argc;
    return argc ? 0 : -LKL_EINVAL;
}

static void task_static_worker(void *argument)
{
    struct task_static_worker *worker = argument;
    int slot = (int)(worker - task_static_workers);
    __atomic_store_n(&worker->tid, lkl_host_ops.thread_self(),
                     __ATOMIC_RELEASE);
    task_applet_prepare[slot]("busybox");
    int applet = task_applet_find[slot](worker->argv[0]);
    worker->exit_armed = 1;
    if (applet < 0) {
        worker->status = 127;
    } else if (!setjmp(worker->exit_jump)) {
        worker->status = task_applet_run[slot](applet, worker->argv) & 255;
    }
    worker->exit_armed = 0;
    if (worker->getopt_locked) {
        worker->getopt_locked = 0;
        __atomic_clear(&task_static_getopt_lock, __ATOMIC_RELEASE);
    }
    for (int fd = 0; fd < 3; fd++) {
        if (worker->fd_map[fd] >= 0) {
            lkl_sys_close((unsigned int)worker->fd_map[fd]);
            worker->fd_map[fd] = -1;
        }
    }
    __atomic_store_n(&worker->done, 1, __ATOMIC_RELEASE);
    void (*sigchld_handler)(int) = task_user_signal_handlers[SIGCHLD];
    if (sigchld_handler && sigchld_handler != SIG_IGN &&
        sigchld_handler != SIG_DFL)
        sigchld_handler(SIGCHLD);
}

static int task_static_start(struct task_static_worker *worker, char **argv,
                             int input, int output, int error)
{
    int result = task_static_copy_argv(worker, argv);
    if (result < 0) {
        task_static_release(worker);
        return result;
    }
    if (!task_static_command_allowed(worker->argv[0])) {
        task_static_release(worker);
        return -LKL_ENOENT;
    }
    int source[3] = { input, output, error };
    for (int fd = 0; fd < 3; fd++) {
        long duplicate = lkl_sys_dup((unsigned int)source[fd]);
        if (duplicate < 0) {
            task_static_release(worker);
            return (int)duplicate;
        }
        worker->fd_map[fd] = (int)duplicate;
    }
    lkl_thread_t tid = lkl_host_ops.thread_create(task_static_worker, worker);
    if (!tid) {
        task_static_release(worker);
        return -LKL_EAGAIN;
    }
    __atomic_store_n(&worker->tid, tid, __ATOMIC_RELEASE);
    __atomic_add_fetch(&task_static_started, 1U, __ATOMIC_RELAXED);
    return (int)tid;
}

static int task_static_join(struct task_static_worker *worker, int *status)
{
    if (!worker || !worker->in_use || worker->reaped) return -LKL_ECHILD;
    if (lkl_host_ops.thread_join(worker->tid)) return -LKL_ECHILD;
    worker->reaped = 1;
    int code = worker->status & 255;
    if (status) *status = code << 8;
    task_static_release(worker);
    return code;
}

int luna_bb_run_static(char **argv)
{
    struct task_static_worker *worker = task_static_allocate();
    if (!worker) return -EAGAIN;
    int pid = task_static_start(worker, argv, 0, 1, 2);
    if (pid < 0) return pid == -LKL_ENOENT ? -ENOENT : -EAGAIN;
    return task_static_join(worker, NULL);
}

long luna_bb_spawn_static(char **argv)
{
    struct task_static_worker *worker = task_static_allocate();
    if (!worker) return -LKL_EAGAIN;
    int pid = task_static_start(worker, argv, 0, 1, 2);
    if (pid > 0)
        __atomic_add_fetch(&task_static_backgrounds, 1U, __ATOMIC_RELAXED);
    return pid;
}

int luna_bb_run_pipeline(int count, char ***commands)
{
    if (count < 2 || count > LUNA_STATIC_WORKERS) return -EINVAL;
    for (int i = 0; i < count; i++) {
        if (!commands[i] || !commands[i][0] ||
            !task_static_command_allowed(commands[i][0]))
            return -ENOENT;
    }
    int pipes[LUNA_STATIC_WORKERS - 1][2];
    memset(pipes, -1, sizeof(pipes));
    for (int i = 0; i < count - 1; i++) {
        long result = lkl_sys_pipe2(pipes[i], 0);
        if (result < 0) {
            task_user_errno = (int)-result;
            goto fail_pipes;
        }
    }
    struct task_static_worker *workers[LUNA_STATIC_WORKERS] = { 0 };
    for (int i = 0; i < count; i++) {
        workers[i] = task_static_allocate();
        if (!workers[i]) goto fail_workers;
        int input = i == 0 ? 0 : pipes[i - 1][0];
        int output = i == count - 1 ? 1 : pipes[i][1];
        int pid = task_static_start(workers[i], commands[i], input, output, 2);
        if (pid < 0) goto fail_workers;
    }
    __atomic_add_fetch(&task_static_pipelines, 1U, __ATOMIC_RELAXED);
    for (int i = 0; i < count - 1; i++) {
        lkl_sys_close((unsigned int)pipes[i][0]);
        lkl_sys_close((unsigned int)pipes[i][1]);
        pipes[i][0] = pipes[i][1] = -1;
    }
    int status = 0;
    for (int i = 0; i < count; i++) {
        int code = task_static_join(workers[i], NULL);
        if (i == count - 1) status = code;
    }
    return status;

fail_workers:
    for (int i = 0; i < count; i++) {
        if (!workers[i]) continue;
        if (workers[i]->tid) task_static_join(workers[i], NULL);
        else task_static_release(workers[i]);
    }
fail_pipes:
    for (int i = 0; i < count - 1; i++) {
        if (pipes[i][0] >= 0) lkl_sys_close((unsigned int)pipes[i][0]);
        if (pipes[i][1] >= 0) lkl_sys_close((unsigned int)pipes[i][1]);
    }
    return -1;
}

pid_t luna_bb_waitpid(pid_t pid, int *status, int options)
{
    for (;;) {
        struct task_static_worker *selected = NULL;
        int matching = 0;
        for (int i = 0; i < LUNA_STATIC_WORKERS; i++) {
            struct task_static_worker *worker = &task_static_workers[i];
            if (!worker->in_use || worker->reaped) continue;
            if (pid > 0 && worker->tid != (lkl_thread_t)pid) continue;
            matching = 1;
            if (worker->done) {
                selected = worker;
                break;
            }
        }
        if (selected) {
            pid_t result = (pid_t)selected->tid;
            if (task_static_join(selected, status) < 0) {
                task_user_errno = ECHILD;
                return -1;
            }
            return result;
        }
        if (!matching) {
            task_user_errno = ECHILD;
            return -1;
        }
        if (options & WNOHANG) return 0;
        seL4_Yield();
    }
}

int luna_bb_pipe(int pipefd[2])
{
    return (int)bb_result(lkl_sys_pipe2(pipefd, 0));
}

__attribute__((noreturn)) void luna_bb__exit(int status)
{
    struct task_static_worker *worker = bb_current_worker();
    if (worker) {
        worker->status = status & 255;
        if (worker->exit_armed) longjmp(worker->exit_jump, 1);
        for (;;) seL4_Yield();
    }
    task_user_status = status & 255;
    if (task_user_exit_armed) longjmp(task_user_exit, 1);
    for (;;) seL4_Yield();
}

__attribute__((noreturn)) void luna_bb_exit(int status)
{
    luna_bb__exit(status);
}

static int verify_file(const char *path, const char *expected)
{
    char buffer[96];
    size_t expected_length = strlen(expected);
    long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
    if (fd < 0) return -1;
    long length = lkl_sys_read((unsigned int)fd, buffer, sizeof(buffer));
    long close_result = lkl_sys_close((unsigned int)fd);
    return length == (long)expected_length && close_result == 0 &&
           !memcmp(buffer, expected, expected_length) ? 0 : -1;
}

static void task_busybox_worker(void *arg)
{
    (void)arg;
    char *argv[] = { "busybox", "cat", "/tmp/x", NULL };
    task_user_status = 255;
    task_user_exit_armed = 1;
    if (!setjmp(task_user_exit)) {
        int result = lbb_main(argv);
        task_user_status = result & 255;
    }
    task_user_exit_armed = 0;
}

static void task_user_heap_reset(void)
{
    task_user_heap_lock = 0;
    task_user_heap_current = 0;
    task_user_heap_peak = 0;
    memset(task_user_heap, 0, sizeof(task_user_heap));
    task_user_heap_head = (void *)task_user_heap;
    task_user_heap_head->size = sizeof(task_user_heap) -
                               sizeof(*task_user_heap_head);
    task_user_heap_head->next = NULL;
    task_user_heap_head->free = 1;
}

static int task_block_drop_cache(int fd)
{
    lkl_sys_sync();
    long drop = lkl_sys_open("/proc/sys/vm/drop_caches", LKL_O_WRONLY, 0);
    if (drop >= 0) {
        static const char command[] = "3\n";
        long written = lkl_sys_write((unsigned int)drop, (void *)command,
                                     sizeof(command) - 1U);
        long closed = lkl_sys_close((unsigned int)drop);
        if (written == (long)(sizeof(command) - 1U) && !closed) return 0;
    }
    long parameters[6] = {
        fd, 0, (long)LUNA_BLOCK_BENCH_BYTES, 4 /* POSIX_FADV_DONTNEED */
    };
    long result = lkl_syscall(__lkl__NR_fadvise64, parameters);
    if (result < 0)
        lkl_printf("luna-lkl-task: block cache drop failed: %ld\n", result);
    return result < 0 ? -1 : 0;
}

static int task_block_benchmark(void)
{
    static const char path[] = "/luna-block-benchmark";
    char buffer[LUNA_BLOCK_BENCH_BLOCK];
    long fd = lkl_sys_open(path, LKL_O_RDWR | LKL_O_CREAT | LKL_O_TRUNC,
                           0644);
    if (fd < 0) return -1;

    unsigned long long start = luna_lkl_task_time();
    for (unsigned block = 0; block < LUNA_BLOCK_BENCH_OPS; block++) {
        memset(buffer, (unsigned char)block, sizeof(buffer));
        if (lkl_sys_write((unsigned int)fd, buffer, sizeof(buffer)) !=
            (long)sizeof(buffer)) goto fail;
    }
    if (lkl_sys_fsync((unsigned int)fd)) goto fail;
    unsigned long long sequential_write = luna_lkl_task_time() - start;

    if (lkl_sys_lseek((unsigned int)fd, 0, LKL_SEEK_SET) < 0) goto fail;
    start = luna_lkl_task_time();
    for (unsigned block = 0; block < LUNA_BLOCK_BENCH_OPS; block++) {
        if (lkl_sys_read((unsigned int)fd, buffer, sizeof(buffer)) !=
            (long)sizeof(buffer)) goto fail;
        for (size_t i = 0; i < sizeof(buffer); i++)
            if ((unsigned char)buffer[i] != (unsigned char)block) goto fail;
    }
    unsigned long long hot_read = luna_lkl_task_time() - start;

    start = luna_lkl_task_time();
    for (unsigned operation = 0; operation < LUNA_BLOCK_BENCH_OPS;
         operation++) {
        unsigned block = (operation * 73U) % LUNA_BLOCK_BENCH_OPS;
        memset(buffer, (unsigned char)(block ^ 0xa5U), sizeof(buffer));
        if (lkl_sys_lseek((unsigned int)fd,
                          (long long)block * LUNA_BLOCK_BENCH_BLOCK,
                          LKL_SEEK_SET) < 0 ||
            lkl_sys_write((unsigned int)fd, buffer, sizeof(buffer)) !=
                (long)sizeof(buffer)) goto fail;
    }
    if (lkl_sys_fsync((unsigned int)fd)) goto fail;
    unsigned long long random_write = luna_lkl_task_time() - start;

    start = luna_lkl_task_time();
    for (unsigned operation = 0; operation < LUNA_BLOCK_BENCH_OPS;
         operation++) {
        unsigned block = (operation * 73U) % LUNA_BLOCK_BENCH_OPS;
        if (lkl_sys_lseek((unsigned int)fd,
                          (long long)block * LUNA_BLOCK_BENCH_BLOCK,
                          LKL_SEEK_SET) < 0 ||
            lkl_sys_read((unsigned int)fd, buffer, sizeof(buffer)) !=
                (long)sizeof(buffer)) goto fail;
        for (size_t i = 0; i < sizeof(buffer); i++)
            if ((unsigned char)buffer[i] !=
                (unsigned char)(block ^ 0xa5U)) goto fail;
    }
    unsigned long long random_read = luna_lkl_task_time() - start;

    if (task_block_drop_cache((int)fd) ||
        lkl_sys_lseek((unsigned int)fd, 0, LKL_SEEK_SET) < 0) goto fail;
    start = luna_lkl_task_time();
    for (unsigned block = 0; block < LUNA_BLOCK_BENCH_OPS; block++) {
        if (lkl_sys_read((unsigned int)fd, buffer, sizeof(buffer)) !=
            (long)sizeof(buffer)) goto fail;
        for (size_t i = 0; i < sizeof(buffer); i++)
            if ((unsigned char)buffer[i] !=
                (unsigned char)(block ^ 0xa5U)) goto fail;
    }
    unsigned long long cold_read = luna_lkl_task_time() - start;
    if (lkl_sys_close((unsigned int)fd) || !sequential_write ||
        !cold_read || !hot_read || !random_write || !random_read) return -1;
    lkl_printf("LUNA_BLOCK_BENCHMARK_OK bytes=%lu seq_write_ns=%llu "
               "cold_read_ns=%llu hot_read_ns=%llu random_ops=%lu "
               "random_write_ns=%llu random_read_ns=%llu\n",
               (unsigned long)LUNA_BLOCK_BENCH_BYTES, sequential_write,
               cold_read, hot_read, (unsigned long)LUNA_BLOCK_BENCH_OPS,
               random_write, random_read);
    return 0;

fail:
    lkl_sys_close((unsigned int)fd);
    return -1;
}

static int task_pipeline_benchmark(void)
{
    static const char path[] = "/run/luna-pipeline-benchmark";
    static const char output_path[] = "/run/luna-pipeline-output";
    char buffer[LUNA_BLOCK_BENCH_BLOCK];
    memset(buffer, 0x5a, sizeof(buffer));
    long fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC,
                           0600);
    if (fd < 0) return -1;
    for (size_t written = 0; written < LUNA_PIPELINE_BENCH_BYTES;
         written += sizeof(buffer)) {
        if (lkl_sys_write((unsigned int)fd, buffer, sizeof(buffer)) !=
            (long)sizeof(buffer)) {
            lkl_sys_close((unsigned int)fd);
            return -1;
        }
    }
    if (lkl_sys_close((unsigned int)fd)) return -1;

    unsigned long long samples[LUNA_PIPELINE_BENCH_SAMPLES];
    task_stdio_read_calls = 0;
    task_stdio_read_bytes = 0;
    task_stdio_write_calls = 0;
    task_stdio_write_bytes = 0;
    task_stdio_benchmarking = 1;
    int result = 0;
    for (unsigned sample = 0; sample < LUNA_PIPELINE_BENCH_SAMPLES;
         sample++) {
        char *cat_argv[] = { "cat", (char *)path, NULL };
        char dd_output[] = "of=/run/luna-pipeline-output";
        char *dd_argv[] = { "dd", dd_output, "bs=65536", NULL };
        char **commands[] = { cat_argv, dd_argv };
        unsigned long long start = luna_lkl_task_time();
        result = luna_bb_run_pipeline(2, commands);
        samples[sample] = luna_lkl_task_time() - start;
        if (result || !samples[sample]) {
            result = -1;
            break;
        }
        lkl_printf("LUNA_PIPELINE_SAMPLE ns=%llu\n", samples[sample]);
    }
    task_stdio_benchmarking = 0;
    struct lkl_stat output_stat;
    if (result || lkl_sys_stat(output_path, &output_stat) ||
        output_stat.st_size != LUNA_PIPELINE_BENCH_BYTES) {
        lkl_sys_unlink(output_path);
        return -1;
    }
    lkl_sys_unlink(output_path);

    /* Exercise the FILE-backed consumer that previously fell into a
     * byte-at-a-time fread/getc loop. */
    char *cat_argv[] = { "cat", (char *)path, NULL };
    char *wc_argv[] = { "wc", "-c", NULL };
    char **wc_commands[] = { cat_argv, wc_argv };
    if (luna_bb_run_pipeline(2, wc_commands)) {
        lkl_sys_unlink(path);
        return -1;
    }
    lkl_sys_unlink(path);

    unsigned long long ordered[LUNA_PIPELINE_BENCH_SAMPLES];
    memcpy(ordered, samples, sizeof(ordered));
    for (unsigned i = 1; i < LUNA_PIPELINE_BENCH_SAMPLES; i++) {
        unsigned long long value = ordered[i];
        unsigned j = i;
        while (j && ordered[j - 1] > value) {
            ordered[j] = ordered[j - 1];
            j--;
        }
        ordered[j] = value;
    }
    unsigned long long p50 = ordered[(LUNA_PIPELINE_BENCH_SAMPLES - 1) / 2];
    unsigned long long p95 = ordered[LUNA_PIPELINE_BENCH_SAMPLES - 1];
    unsigned long long p99 = p95;
    unsigned long long bytes_per_second =
        (LUNA_PIPELINE_BENCH_BYTES * 1000000000ULL) / p50;
    lkl_printf("LUNA_PIPELINE_BENCHMARK_OK bytes=%lu elapsed_ns=%llu "
               "bytes_per_sec=%llu samples=%u p50_ns=%llu p95_ns=%llu "
               "p99_ns=%llu read_calls=%llu read_bytes=%llu "
               "write_calls=%llu write_bytes=%llu\n",
               (unsigned long)LUNA_PIPELINE_BENCH_BYTES, p50,
               bytes_per_second, LUNA_PIPELINE_BENCH_SAMPLES, p50, p95,
               p99, task_stdio_read_calls, task_stdio_read_bytes,
               task_stdio_write_calls, task_stdio_write_bytes);
    return 0;
}

static int task_user_heap_self_test(void)
{
    task_user_heap_reset();
    void *first = luna_bb_malloc(128);
    void *second = luna_bb_malloc(256);
    void *third = luna_bb_malloc(64);
    if (!first || !second || !third ||
        ((uintptr_t)first & (LUNA_USER_ALIGNMENT - 1U)) ||
        ((uintptr_t)second & (LUNA_USER_ALIGNMENT - 1U)) ||
        ((uintptr_t)third & (LUNA_USER_ALIGNMENT - 1U)))
        return -1;
    memset(first, 0xa5, 128);
    luna_bb_free(second);
    void *reuse = luna_bb_malloc(128);
    if (reuse != second) return -1;
    luna_bb_free(reuse);
    void *grown = luna_bb_realloc(first, 320);
    if (!grown) return -1;
    for (size_t i = 0; i < 128; i++)
        if (((unsigned char *)grown)[i] != 0xa5) return -1;
    luna_bb_free(third);
    luna_bb_free(grown);
    if (task_user_heap_head != (void *)task_user_heap ||
        !task_user_heap_head->free || task_user_heap_head->next ||
        task_user_heap_head->size != sizeof(task_user_heap) -
                                    sizeof(*task_user_heap_head))
        return -1;
    task_user_heap_reset();
    return 0;
}

static void task_busybox_shell_worker(void *arg)
{
    (void)arg;
    char *argv[] = { "busybox", "sh", "-i", "+m", NULL };
    task_user_status = 255;
    task_user_exit_armed = 1;
    if (!setjmp(task_user_exit)) {
        int result = lbb_main(argv);
        task_user_status = result & 255;
    }
    task_user_exit_armed = 0;
}

int luna_lkl_task_user_smoke(void)
{
    if (verify_file(LUNA_BUSYBOX_PATH, LUNA_BUSYBOX_MANIFEST)) {
        lkl_printf("luna-lkl-task: static BusyBox manifest invalid\n");
        return -1;
    }
    task_user_forbidden_calls = 0;
    task_user_errno = 0;
    if (task_block_benchmark()) {
        lkl_printf("luna-lkl-task: block benchmark failed\n");
        return -1;
    }
    if (task_user_heap_self_test()) {
        lkl_printf("luna-lkl-task: BusyBox heap self-test failed\n");
        return -1;
    }
    long fd = lkl_sys_open("/tmp/x", LKL_O_WRONLY | LKL_O_CREAT |
                          LKL_O_TRUNC, 0644);
    static const char expected[] = "ok\n";
    if (fd < 0 || lkl_sys_write((unsigned int)fd, (void *)expected,
                                sizeof(expected) - 1U) !=
                  (long)(sizeof(expected) - 1U) ||
        lkl_sys_close((unsigned int)fd))
        return -1;
    lkl_thread_t pid = lkl_host_ops.thread_create(task_busybox_worker, NULL);
    if (!pid || lkl_host_ops.thread_join(pid) || task_user_status != 0 ||
        task_user_forbidden_calls || verify_file("/tmp/x", "ok\n")) {
        lkl_printf("luna-lkl-task: BusyBox spawn/wait failed pid=%lu "
                   "status=%d forbidden=%d\n", (unsigned long)pid,
                   task_user_status, task_user_forbidden_calls);
        return -1;
    }
    lkl_printf("LUNA_STATIC_USER_OK path=%s abi=1\n", LUNA_BUSYBOX_PATH);
    lkl_printf("LUNA_BUSYBOX_HEAP_OK bytes=%lu\n",
               (unsigned long)sizeof(task_user_heap));
    lkl_printf("LUNA_SPAWN_WAIT_OK pid=%lu status=%d\n",
               (unsigned long)pid, task_user_status);
    lkl_printf("LUNA_BUSYBOX_OK command=cat /tmp/x\n");
    return 0;
}

int luna_lkl_task_user_shell(void)
{
    static char ps1[] = "PS1=luna-ash# ";
    static char home[] = "HOME=/";
    static char path[] = "PATH=/bin:/sbin";
    static char pwd[] = "PWD=/";
    if (putenv(ps1) || putenv(home) || putenv(path) || putenv(pwd))
        return -1;

    task_user_forbidden_calls = 0;
    task_user_errno = 0;
    memset(task_static_workers, 0, sizeof(task_static_workers));
    memset(task_user_signal_handlers, 0, sizeof(task_user_signal_handlers));
    task_static_started = 0;
    task_static_pipelines = 0;
    task_static_backgrounds = 0;
    task_static_getopt_lock = 0;
    task_static_current = 0;
    task_static_peak = 0;
    task_user_heap_reset();
    if (task_pipeline_benchmark()) {
        lkl_printf("luna-lkl-task: pipeline benchmark failed\n");
        return -1;
    }
    memset(task_static_workers, 0, sizeof(task_static_workers));
    memset(task_user_signal_handlers, 0, sizeof(task_user_signal_handlers));
    task_static_started = 0;
    task_static_pipelines = 0;
    task_static_backgrounds = 0;
    task_static_getopt_lock = 0;
    task_static_current = 0;
    task_static_peak = 0;
    task_user_heap_reset();
    lkl_printf("LUNA_BUSYBOX_INTERACTIVE_READY prompt=luna-ash#\n");
    lkl_thread_t pid = lkl_host_ops.thread_create(task_busybox_shell_worker,
                                                   NULL);
    if (!pid || lkl_host_ops.thread_join(pid) || task_user_status != 0 ||
        task_user_forbidden_calls) {
        lkl_printf("luna-lkl-task: interactive BusyBox failed pid=%lu "
                   "status=%d forbidden=%d errno=%d\n",
                   (unsigned long)pid, task_user_status,
                   task_user_forbidden_calls, task_user_errno);
        return -1;
    }
    for (int i = 0; i < LUNA_STATIC_WORKERS; i++) {
        if (task_static_workers[i].in_use &&
            task_static_join(&task_static_workers[i], NULL) < 0)
            return -1;
    }
    lkl_sys_sync();
    lkl_printf("LUNA_STATIC_RUNTIME_OK workers=%u pipelines=%u background=%u\n",
               task_static_started, task_static_pipelines,
               task_static_backgrounds);
    lkl_printf("LUNA_BUSYBOX_INTERACTIVE_OK status=%d forbidden=%d\n",
               task_user_status, task_user_forbidden_calls);
    lkl_printf("LUNA_USER_RESOURCE_PEAK_OK heap_bytes=%lu workers=%u\n",
               (unsigned long)task_user_heap_peak, task_static_peak);
    return 0;
}
