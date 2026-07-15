/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LUNA_ISOLATION_H
#define LUNA_ISOLATION_H

#include <simple/simple.h>
#include <vka/vka.h>
#include <vspace/vspace.h>

int luna_isolation_smoke(simple_t *simple, vka_t *vka,
                         vspace_t *manager_vspace,
                         unsigned long long tsc_frequency);

#endif /* LUNA_ISOLATION_H */
