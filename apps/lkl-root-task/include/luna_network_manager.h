/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_NETWORK_MANAGER_H
#define LUNA_NETWORK_MANAGER_H

#include "luna_isolation_protocol.h"

#include <sel4utils/process.h>
#include <simple/simple.h>
#include <vka/vka.h>
#include <vspace/vspace.h>

struct luna_net_mapping {
    reservation_t reservation;
    cspacepath_t frame_caps[LUNA_NET_IO_PAGES];
    unsigned char cap_allocated[LUNA_NET_IO_PAGES];
    size_t mapped_pages;
    int reserved;
    seL4_CPtr child_rx_ntfn;
};

int luna_network_manager_init(simple_t *simple, vka_t *vka,
                              vspace_t *manager_vspace);
int luna_network_map_child(vka_t *vka, vspace_t *manager_vspace,
                           sel4utils_process_t *process,
                           struct luna_net_mapping *mapping,
                           int activate);
void luna_network_deactivate_child(void);
int luna_network_verify_irq(void);
int luna_network_service(seL4_CPtr command_ep, seL4_Word event,
                         seL4_Word length_word);

#endif /* LUNA_NETWORK_MANAGER_H */
