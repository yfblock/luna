/* SPDX-License-Identifier: GPL-2.0 */
/* Prepare the LKL console as fd 0/1/2 for the interactive BusyBox ash. */
#include "luna_shell.h"
#include <stdio.h>
#include <lkl.h>
#include <lkl/asm/syscalls.h>

int luna_shell_prepare(int console_ready)
{
    lkl_sys_mkdir("/dev", 0755);
    lkl_sys_mknod("/dev/ttyLKL0", LKL_S_IFCHR | 0666, (240 << 8) | 0);
    lkl_sys_close(0);
    lkl_sys_close(1);
    lkl_sys_close(2);
    long tfd = lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);
    printf("luna: /dev/ttyLKL0 first open fd=%ld\n", tfd);
    if (tfd != 0 || !console_ready) return -1;
    long outfd = lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);
    long errfd = lkl_sys_open("/dev/ttyLKL0", LKL_O_RDWR, 0);
    if (outfd != 1 || errfd != 2) return -1;
    lkl_sys_mkdir("/proc", 0555);
    lkl_sys_mkdir("/tmp", 0755);
    if (lkl_sys_mount("proc", "/proc", "proc", 0, NULL) < 0) return -1;
    return 0;
}
