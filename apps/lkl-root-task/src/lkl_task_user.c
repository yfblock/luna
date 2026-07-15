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
#include <errno.h>
#include <fcntl.h>
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
#include <termios.h>
#include <unistd.h>

#define LUNA_BUSYBOX_PATH "/bin/busybox"
#define LUNA_BUSYBOX_MANIFEST "LUNA-STATIC-ABI-1\nbusybox-1.36.1\n"
#define LUNA_USER_HEAP_SIZE (1024UL * 1024UL)
#define LUNA_USER_ALIGNMENT 16UL
#define LUNA_USER_MIN_SPLIT 16UL

extern int lbb_main(char **argv);

static jmp_buf task_user_exit;
static volatile int task_user_exit_armed;
static volatile int task_user_status;
static volatile int task_user_forbidden_calls;
static int task_user_errno;
static unsigned char task_user_heap[LUNA_USER_HEAP_SIZE]
    __attribute__((aligned(16)));

struct task_user_block {
    size_t size;
    struct task_user_block *next;
    int free;
    size_t reserved;
};

static struct task_user_block *task_user_heap_head;

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
    return (int)bb_result(lkl_sys_open(path, flags, mode));
}

int luna_bb_close(int fd)
{
    return (int)bb_result(lkl_sys_close((unsigned int)fd));
}

ssize_t luna_bb_read(int fd, void *buffer, size_t count)
{
    return (ssize_t)bb_result(lkl_sys_read((unsigned int)fd, buffer, count));
}

ssize_t luna_bb_write(int fd, const void *buffer, size_t count)
{
    return (ssize_t)bb_result(lkl_sys_write((unsigned int)fd,
                                            (void *)buffer, count));
}

off_t luna_bb_lseek(int fd, off_t offset, int whence)
{
    return (off_t)bb_result(lkl_sys_lseek((unsigned int)fd, offset, whence));
}

int luna_bb_dup(int fd)
{
    return (int)bb_result(lkl_sys_dup((unsigned int)fd));
}

int luna_bb_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd) return oldfd;
    return (int)bb_result(lkl_sys_dup3((unsigned int)oldfd,
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
    return (int)bb_result(lkl_sys_fcntl((unsigned int)fd, command,
                                        argument));
}

int luna_bb_fstat(int fd, struct stat *target)
{
    struct lkl_stat source;
    long result = lkl_sys_fstat((unsigned int)fd, &source);
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

int luna_bb_chdir(const char *path)
{
    return (int)bb_result(lkl_sys_chdir(path));
}

int luna_bb_fchdir(int fd)
{
    return (int)bb_result(lkl_sys_fchdir((unsigned int)fd));
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

int luna_bb_isatty(int fd)
{
    struct termios attributes;
    long result = lkl_sys_ioctl((unsigned int)fd, TCGETS,
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
    return (int)bb_result(lkl_sys_ioctl((unsigned int)fd, request, argument));
}

int luna_bb_tcgetattr(int fd, struct termios *termios)
{
    return (int)bb_result(lkl_sys_ioctl((unsigned int)fd, TCGETS,
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
    return (int)bb_result(lkl_sys_ioctl((unsigned int)fd, request,
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
    return (int)bb_result(lkl_sys_fsync((unsigned int)fd));
}

int luna_bb_fdatasync(int fd)
{
    return (int)bb_result(lkl_sys_fdatasync((unsigned int)fd));
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
    return 2;
}
pid_t luna_bb_getppid(void) { return 1; }
uid_t luna_bb_geteuid(void) { return 0; }
gid_t luna_bb_getegid(void) { return 0; }
int luna_bb_mallopt(int parameter, int value)
{
    (void)parameter;
    (void)value;
    return 1;
}

void *luna_bb_malloc(size_t size)
{
    if (!size || size > SIZE_MAX - (LUNA_USER_ALIGNMENT - 1U)) {
        task_user_errno = ENOMEM;
        return NULL;
    }
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
        return block + 1;
    }
    task_user_errno = ENOMEM;
    return NULL;
}

void luna_bb_free(void *pointer)
{
    if (!pointer) return;
    struct task_user_block *previous = NULL;
    struct task_user_block *block = task_user_heap_head;
    while (block && block + 1 != pointer) {
        previous = block;
        block = block->next;
    }
    if (!block || block->free) return;
    block->free = 1;
    if (block->next && block->next->free) {
        block->size += sizeof(*block) + block->next->size;
        block->next = block->next->next;
    }
    if (previous && previous->free) {
        previous->size += sizeof(*previous) + block->size;
        previous->next = block->next;
    }
}

void *luna_bb_realloc(void *pointer, size_t size)
{
    if (!pointer) return luna_bb_malloc(size);
    if (!size) {
        luna_bb_free(pointer);
        return NULL;
    }
    struct task_user_block *block = task_user_heap_head;
    while (block && block + 1 != pointer) block = block->next;
    if (!block || block->free) {
        task_user_errno = EINVAL;
        return NULL;
    }
    if (size <= block->size) {
        return pointer;
    }
    void *replacement = luna_bb_malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, block->size);
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
    (void)signal_number;
    return 0;
}

void (*luna_bb_signal(int signal_number, void (*handler)(int)))(int)
{
    (void)signal_number;
    (void)handler;
    return SIG_DFL;
}

int luna_bb_sigaction(int signal_number, const struct sigaction *action,
                      struct sigaction *old_action)
{
    (void)signal_number;
    (void)action;
    if (old_action) {
        memset(old_action, 0, sizeof(*old_action));
        old_action->sa_handler = SIG_DFL;
    }
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
pid_t luna_bb_waitpid(pid_t pid, int *status, int options)
{
    (void)pid; (void)status; (void)options;
    return (pid_t)bb_forbidden();
}
int luna_bb_pipe(int pipefd[2])
{
    (void)pipefd;
    return bb_forbidden();
}

__attribute__((noreturn)) void luna_bb__exit(int status)
{
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
    memset(task_user_heap, 0, sizeof(task_user_heap));
    task_user_heap_head = (void *)task_user_heap;
    task_user_heap_head->size = sizeof(task_user_heap) -
                               sizeof(*task_user_heap_head);
    task_user_heap_head->next = NULL;
    task_user_heap_head->free = 1;
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
    lkl_sys_sync();
    lkl_printf("LUNA_BUSYBOX_INTERACTIVE_OK status=%d forbidden=%d\n",
               task_user_status, task_user_forbidden_calls);
    return 0;
}
