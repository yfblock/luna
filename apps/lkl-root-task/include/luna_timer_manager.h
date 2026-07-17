/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_TIMER_MANAGER_H
#define LUNA_TIMER_MANAGER_H

#include "luna_isolation_protocol.h"

#include <sel4utils/thread.h>
#include <simple/simple.h>
#include <vka/vka.h>
#include <vspace/vspace.h>

int luna_timer_manager_init(simple_t *simple, vka_t *vka,
                            vspace_t *manager_vspace,
                            unsigned long long tsc_frequency);
void luna_timer_manager_deactivate(void);
void luna_timer_manager_destroy(vka_t *vka, vspace_t *manager_vspace);
int luna_timer_manager_service(seL4_CPtr command_ep, seL4_CPtr child_ntfn,
                               seL4_Word event, seL4_Word generation,
                               seL4_Word timeout_ns);
int luna_timer_manager_schedule_network(seL4_CPtr poll_ntfn,
                                        unsigned long long timeout_ns);
int luna_timer_manager_cancel_network(void);
void luna_timer_manager_report(void);

#endif /* LUNA_TIMER_MANAGER_H */
