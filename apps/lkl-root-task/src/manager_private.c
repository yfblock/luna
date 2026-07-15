/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This object is linked only into the manager and deliberately appears after
 * the root-hosted LKL heap in the root executable.  Keeping the probe in a
 * separate tail object prevents growth of the child BSS from accidentally
 * mapping the same virtual address and invalidating the isolation test.
 */
#include "luna_manager_private.h"

seL4_Word luna_manager_private_page[
    BIT(seL4_PageBits) / sizeof(seL4_Word)]
    __attribute__((aligned(BIT(seL4_PageBits))));
