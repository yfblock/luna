/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_SHELL_H
#define LUNA_SHELL_H

typedef unsigned long long (*luna_shell_time_fn_t)(void);

int luna_shell_prepare(int console_ready);
void luna_shell_run(luna_shell_time_fn_t time_fn);

#endif /* LUNA_SHELL_H */
