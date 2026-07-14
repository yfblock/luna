/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shell.c — 宿主侧交互 shell（经 LKL 虚拟串口 /dev/ttyLKL0）。
 *
 * 输入：lkl_sys_read(0, ...) —— 经 LKL tty，ldisc(n_tty) 自动回显+行编辑，read 返回整行。
 * 输出：lkl_sys_write(1, ...) —— 经 LKL tty write → lkl_ops->print → seL4_DebugPutChar。
 * 命令：经 lkl_sys_* 在 LKL 内核上执行。
 *
 * fd 0/1/2 由 main.c 打开 /dev/ttyLKL0 并 dup2 设置。
 */
#include "sel4_lkl_host.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <lkl.h>
#include <lkl/asm/syscalls.h>

static long sh_write(const char *s, long n) { return lkl_sys_write(1, (void *)s, n < 0 ? 0 : n); }
static void sh_puts(const char *s) { sh_write(s, (long)strlen(s)); }
static void sh_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sh_write(buf, n);
}

/* 读一行（ldisc 已行编辑并回显，read 返回含 \n 的整行） */
static int sh_readline(char *buf, int max)
{
    long n = lkl_sys_read(0, buf, max - 1);
    if (n <= 0) return 0;
    if (buf[n - 1] == '\n') n--;        /* 去掉行尾 \n */
    if (n > 0 && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    return (int)n;
}

static int split_args(char *line, char **argv, int maxa)
{
    int argc = 0; char *p = line;
    while (*p && argc < maxa - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static void cmd_ls(const char *path)
{
    long fd = lkl_sys_open(path, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    if (fd < 0) { sh_printf("ls: %s: %ld\n", path, fd); return; }
    char buf[512]; long n;
    while ((n = lkl_sys_getdents64((int)fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct lkl_linux_dirent64 *d = (struct lkl_linux_dirent64 *)(buf + off);
            const char *t = d->d_type == 4 ? "d" : d->d_type == 8 ? "f" :
                            d->d_type == 10 ? "l" : "?";
            sh_printf("%s %s\n", t, d->d_name);
            off += d->d_reclen;
        }
    }
    lkl_sys_close((int)fd);
}

static void cmd_cat(const char *path)
{
    long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
    if (fd < 0) { sh_printf("cat: %s: %ld\n", path, fd); return; }
    char buf[512]; long n;
    while ((n = lkl_sys_read((int)fd, buf, sizeof(buf))) > 0)
        sh_write(buf, n);
    lkl_sys_close((int)fd);
    if (n < 0) sh_printf("cat: read err %ld\n", n);
}

static void cmd_stat(const char *path)
{
    struct lkl_stat st;
    if (lkl_sys_stat(path, &st) < 0) { sh_printf("stat: %s: err\n", path); return; }
    sh_printf("  size : %lld\n  mode : %o\n  type : %s\n", (long long)st.st_size,
              st.st_mode & 07777, ((st.st_mode & 0170000) == 0040000) ? "dir" :
              ((st.st_mode & 0170000) == 0100000) ? "reg" : "other");
}

static void cmd_write(const char *path, const char *text)
{
    long fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0644);
    if (fd < 0) { sh_printf("write: %s: %ld\n", path, fd); return; }
    long n = lkl_sys_write((int)fd, (void *)text, strlen(text));
    lkl_sys_close((int)fd);
    sh_printf("wrote %ld bytes to %s\n", n, path);
}

static void prompt(void)
{
    char cwd[256];
    if (lkl_sys_getcwd(cwd, sizeof(cwd)) <= 0) strcpy(cwd, "?");
    sh_printf("lkl:%s# ", cwd);
}

void luna_shell_run(void)
{
    sh_puts("\nluna LKL shell (via /dev/ttyLKL0) — type 'help'\n");
    char line[256]; char *argv[16];
    for (;;) {
        prompt();
        if (sh_readline(line, sizeof(line)) == 0) continue;
        int argc = split_args(line, argv, 16);
        if (argc == 0) continue;
        const char *cmd = argv[0];

        if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) { sh_puts("bye\n"); break; }
        else if (!strcmp(cmd, "help"))
            sh_puts("ls [path] cat <f> cd [path] pwd mkdir <d> rmdir <d> rm <f>\n"
                    "touch <f> write <f> <text...> stat <f> echo <text...>\n"
                    "mount <src> <dir> <fstype>  free  exit\n");
        else if (!strcmp(cmd, "ls")) cmd_ls(argc > 1 ? argv[1] : ".");
        else if (!strcmp(cmd, "cat")) { if (argc > 1) cmd_cat(argv[1]); else sh_puts("cat: need file\n"); }
        else if (!strcmp(cmd, "cd")) { long r = lkl_sys_chdir(argc > 1 ? argv[1] : "/"); if (r<0) sh_printf("cd: %ld\n", r); }
        else if (!strcmp(cmd, "pwd")) { char c[256]; lkl_sys_getcwd(c, sizeof(c)); sh_printf("%s\n", c); }
        else if (!strcmp(cmd, "mkdir")) { if (argc>1){long r=lkl_sys_mkdir(argv[1],0755); if(r<0) sh_printf("mkdir: %ld\n",r);} }
        else if (!strcmp(cmd, "rmdir")) { if (argc>1){long r=lkl_sys_rmdir(argv[1]); if(r<0) sh_printf("rmdir: %ld\n",r);} }
        else if (!strcmp(cmd, "rm"))    { if (argc>1){long r=lkl_sys_unlink(argv[1]); if(r<0) sh_printf("rm: %ld\n",r);} }
        else if (!strcmp(cmd, "touch")) { if (argc>1) lkl_sys_close((int)lkl_sys_open(argv[1], LKL_O_CREAT, 0644)); }
        else if (!strcmp(cmd, "write")) {
            if (argc > 2) {
                char text[200]; text[0]=0;
                for (int i=2;i<argc;i++) snprintf(text+strlen(text),sizeof(text)-strlen(text),"%s%s",argv[i],i+1<argc?" ":"");
                cmd_write(argv[1], text);
            } else sh_puts("write: need <file> <text>\n");
        }
        else if (!strcmp(cmd, "stat")) { if (argc>1) cmd_stat(argv[1]); else sh_puts("stat: need path\n"); }
        else if (!strcmp(cmd, "echo")) { for (int i=1;i<argc;i++) sh_printf("%s%s",argv[i],i+1<argc?" ":""); sh_puts("\n"); }
        else if (!strcmp(cmd, "mount")) {
            if (argc>3){long r=lkl_sys_mount(argv[1],argv[2],argv[3],0,NULL); if(r<0) sh_printf("mount: %ld\n",r); else sh_puts("mounted\n");}
            else sh_puts("mount <src> <dir> <fstype>\n");
        }
        else if (!strcmp(cmd, "free")) cmd_cat("/proc/meminfo");
        else sh_printf("unknown: %s (try help)\n", cmd);
    }
}
