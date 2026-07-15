/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_MANAGER_PRIVATE_H
#define LUNA_MANAGER_PRIVATE_H

#include <sel4/sel4.h>
#include <utils/util.h>

extern seL4_Word luna_manager_private_page[
    BIT(seL4_PageBits) / sizeof(seL4_Word)];

#endif /* LUNA_MANAGER_PRIVATE_H */
