/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Manager-owned QEMU virtio-net backend. The child only receives a bounded
 * packet batch window; PCI I/O authority and all DMA memory stay in the root
 * manager.
 */
#include "luna_network_manager.h"
#include "luna_timer_manager.h"

#include <ethdrivers/helpers.h>
#include <ethdrivers/raw.h>
#include <ethdrivers/virtio_pci.h>
#include <platsupport/io.h>
#include <sel4platsupport/arch/io.h>
#include <sel4platsupport/io.h>
#include <sel4utils/page_dma.h>
#include <sel4utils/thread.h>
#include <utils/util.h>
#include <vka/capops.h>
#include <vka/object.h>
#include <vka/object_capops.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANAGER_NET_IO_ADDR ((void *)(uintptr_t)0x53000000UL)
#define LUNA_NET_PCI_BUS 0U
#define LUNA_NET_PCI_DEVICE 5U
#define LUNA_NET_PCI_FUNCTION 0U
#define LUNA_NET_PCI_VENDOR 0x1af4U
#define LUNA_NET_PCI_DEVICE_ID 0x1000U
#define LUNA_NET_RX_DMA_COUNT 64U
#define LUNA_NET_RX_QUEUE_COUNT 32U
#define LUNA_NET_TX_QUEUE_COUNT 64U
#define LUNA_NET_TX_DMA_COUNT 16U
#define LUNA_NET_POLL_PRIORITY 100U
#define LUNA_NET_POLL_STACK_PAGES 4U
#define LUNA_NET_IRQ_PRIORITY 101U
#define LUNA_NET_IRQ_STACK_PAGES 1U
#define LUNA_NET_TX_RECHECK_NS 5000000ULL
#define LUNA_NET_INTX_BUDGET 16U
#define LUNA_NET_IRQ_BADGE 1UL
#define LUNA_NET_TX_SUBMIT_BADGE 2UL
#define LUNA_NET_TIMER_BADGE 4UL
#define PCI_CONFIG_ADDRESS 0x0cf8U
#define PCI_CONFIG_DATA 0x0cfcU
#define PCI_COMMAND 0x04U
#define PCI_BAR0 0x10U
#define PCI_INTERRUPT_LINE 0x3cU
#define PCI_INTERRUPT_PIN 0x3dU
#define PCI_COMMAND_IO 0x1U
#define PCI_COMMAND_MASTER 0x4U

struct luna_dma_buffer {
    void *virt;
    uintptr_t phys;
    int in_use;
    unsigned queue_index;
};

struct luna_rx_packet {
    size_t length;
    unsigned char data[LUNA_NET_PACKET_SIZE];
};

struct luna_tx_packet {
    size_t length;
    unsigned char data[LUNA_NET_PACKET_SIZE];
};

struct luna_network_state {
    ps_io_ops_t io_ops;
    struct eth_driver driver;
    struct luna_dma_buffer rx_dma[LUNA_NET_RX_DMA_COUNT];
    struct luna_dma_buffer tx_dma[LUNA_NET_TX_DMA_COUNT];
    struct luna_rx_packet rx_queue[LUNA_NET_RX_QUEUE_COUNT];
    struct luna_tx_packet tx_queue[LUNA_NET_TX_QUEUE_COUNT];
    volatile unsigned rx_head;
    volatile unsigned rx_tail;
    volatile unsigned tx_head;
    volatile unsigned tx_tail;
    volatile unsigned tx_submit;
    volatile unsigned tx_pending;
    volatile int tx_stress_gate;
    volatile int child_active;
    volatile int poll_in_driver;
    volatile int notification_masked;
    volatile int backpressure_active;
    volatile uint64_t rx_drops;
    volatile uint64_t rx_high_water;
    volatile uint64_t rx_backpressure;
    volatile uint64_t rx_empty_fetches;
    volatile uint64_t tx_high_water;
    volatile uint64_t tx_backpressure;
    volatile uint64_t tx_driver_retries;
    volatile uint64_t tx_completed;
    volatile uint64_t tx_endpoint_requests;
    volatile uint64_t tx_endpoint_batches;
    volatile uint64_t tx_endpoint_packets;
    volatile uint64_t tx_shared_copies;
    volatile uint64_t tx_kicks;
    volatile uint64_t tx_timeout_retries;
    volatile uint64_t rx_endpoint_requests;
    volatile uint64_t rx_endpoint_batches;
    volatile uint64_t rx_endpoint_packets;
    volatile uint64_t rx_shared_copies;
    volatile uint64_t irq_count;
    volatile uint64_t irq_packets;
    volatile uint64_t irq_coalesced_polls;
    volatile uint64_t irq_coalesced_packets;
    volatile uint64_t irq_budget_exhaustions;
    volatile uint64_t irq_max_packets;
    volatile uint64_t irq_kick_polls;
    volatile uint64_t fallback_polls;
    volatile uint64_t irq_errors;
    vka_object_t rx_ntfn;
    vka_object_t tx_ntfn;
    vka_object_t tx_idle_ntfn;
    vka_object_t poll_kick_ntfn;
    vka_object_t irq_ntfn;
    vka_object_t irq_done_ntfn;
    cspacepath_t irq_handler_path;
    cspacepath_t irq_badged_ntfn_path;
    cspacepath_t timer_badged_ntfn_path;
    sel4utils_thread_t poll_thread;
    sel4utils_thread_t irq_thread;
    unsigned irq_line;
    int irq_mode;
    unsigned char mac[6];
    void *io_mapping;
    reservation_t io_reservation;
    int io_allocated;
    int ready;
};

/* Keep this live pointer in .data, not at the .bss/static-stack boundary. */
static struct luna_network_state *network_state =
    (struct luna_network_state *)(uintptr_t)1;
#define network (*network_state)

static void service_tx_notification(struct luna_network_state *state);

static int rx_queue_full(const struct luna_network_state *state)
{
    unsigned head = __atomic_load_n(&state->rx_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&state->rx_tail, __ATOMIC_ACQUIRE);
    return (head + 1U) % LUNA_NET_RX_QUEUE_COUNT == tail;
}

static int tx_queue_empty(const struct luna_network_state *state)
{
    unsigned head = __atomic_load_n(&state->tx_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&state->tx_tail, __ATOMIC_ACQUIRE);
    return head == tail;
}

static void record_rx_backpressure(struct luna_network_state *state)
{
    int expected = 0;
    if (__atomic_compare_exchange_n(&state->backpressure_active,
                                    &expected, 1, false,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_RELAXED))
        __atomic_fetch_add(&state->rx_backpressure, 1,
                           __ATOMIC_RELAXED);
}

static void signal_rx(struct luna_network_state *state)
{
    if (!__atomic_load_n(&state->notification_masked, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE)) {
        seL4_Signal(state->rx_ntfn.cptr);
    }
}

static uint32_t pci_config_key(unsigned offset)
{
    return 0x80000000U | (LUNA_NET_PCI_BUS << 16) |
           (LUNA_NET_PCI_DEVICE << 11) |
           (LUNA_NET_PCI_FUNCTION << 8) | (offset & ~3U);
}

static int pci_read(unsigned offset, int size, uint32_t *value)
{
    if (ps_io_port_out(&network.io_ops.io_port_ops, PCI_CONFIG_ADDRESS,
                       4, pci_config_key(offset)))
        return -1;
    return ps_io_port_in(&network.io_ops.io_port_ops,
                         PCI_CONFIG_DATA + (offset & 3U), size, value);
}

static int pci_write(unsigned offset, int size, uint32_t value)
{
    if (ps_io_port_out(&network.io_ops.io_port_ops, PCI_CONFIG_ADDRESS,
                       4, pci_config_key(offset)))
        return -1;
    return ps_io_port_out(&network.io_ops.io_port_ops,
                          PCI_CONFIG_DATA + (offset & 3U), size, value);
}

static uintptr_t allocate_rx_buffer(void *cookie, size_t size,
                                    void **buffer_cookie)
{
    struct luna_network_state *state = cookie;
    if (!buffer_cookie || size > LUNA_NET_PACKET_SIZE ||
        !__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE))
        return 0;
    for (unsigned i = 0; i < LUNA_NET_RX_DMA_COUNT; i++) {
        struct luna_dma_buffer *buffer = &state->rx_dma[i];
        if (buffer->in_use) continue;
        if (!buffer->virt) {
            dma_addr_t dma = dma_alloc_pin(&state->io_ops.dma_manager,
                                           LUNA_NET_PACKET_SIZE, 1, 16);
            if (!dma.virt || !dma.phys) return 0;
            buffer->virt = dma.virt;
            buffer->phys = dma.phys;
        }
        buffer->in_use = 1;
        *buffer_cookie = buffer;
        return buffer->phys;
    }
    return 0;
}

static void receive_complete(void *cookie, unsigned int num_buffers,
                             void **cookies, unsigned int *lengths)
{
    struct luna_network_state *state = cookie;
    size_t total = 0;
    for (unsigned i = 0; i < num_buffers; i++) {
        if (!cookies[i] || lengths[i] > LUNA_NET_PACKET_SIZE - total) {
            total = 0;
            break;
        }
        total += lengths[i];
    }
    unsigned head = __atomic_load_n(&state->rx_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&state->rx_tail, __ATOMIC_ACQUIRE);
    unsigned next = (head + 1U) % LUNA_NET_RX_QUEUE_COUNT;
    if (total && total <= LUNA_NET_PACKET_SIZE &&
        __atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE) &&
        next != tail) {
        struct luna_rx_packet *packet = &state->rx_queue[head];
        size_t offset = 0;
        for (unsigned i = 0; i < num_buffers; i++) {
            struct luna_dma_buffer *buffer = cookies[i];
            memcpy(packet->data + offset, buffer->virt, lengths[i]);
            offset += lengths[i];
        }
        packet->length = total;
        __atomic_store_n(&state->rx_head, next, __ATOMIC_RELEASE);
        __atomic_fetch_add(&((struct luna_net_batch_header *)
                                 state->io_mapping)->rx_produced,
                           1, __ATOMIC_RELEASE);
        unsigned used = (next + LUNA_NET_RX_QUEUE_COUNT - tail) %
                        LUNA_NET_RX_QUEUE_COUNT;
        uint64_t high_water = __atomic_load_n(&state->rx_high_water,
                                               __ATOMIC_RELAXED);
        while (used > high_water &&
               !__atomic_compare_exchange_n(&state->rx_high_water,
                                            &high_water, used, false,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) { }
        struct luna_net_batch_header *header = state->io_mapping;
        if (__atomic_exchange_n(&header->rx_waiting, 0,
                                __ATOMIC_ACQ_REL))
            signal_rx(state);
    } else if (total && total <= LUNA_NET_PACKET_SIZE &&
               __atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE)) {
        record_rx_backpressure(state);
        __atomic_fetch_add(&state->rx_drops, 1, __ATOMIC_RELAXED);
    }
    for (unsigned i = 0; i < num_buffers; i++) {
        struct luna_dma_buffer *buffer = cookies[i];
        if (buffer) buffer->in_use = 0;
    }
}

static void transmit_complete(void *cookie, void *buffer_cookie)
{
    struct luna_network_state *state = cookie;
    struct luna_dma_buffer *buffer = NULL;
    for (unsigned i = 0; i < LUNA_NET_TX_DMA_COUNT; i++) {
        if (buffer_cookie == &state->tx_dma[i]) {
            buffer = &state->tx_dma[i];
            break;
        }
    }
    if (!buffer || !buffer->in_use ||
        buffer->queue_index >= LUNA_NET_TX_QUEUE_COUNT) return;
    state->tx_queue[buffer->queue_index].length = 0;
    buffer->in_use = 0;
    __atomic_fetch_add(&state->tx_completed, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&state->tx_pending, 1U, __ATOMIC_ACQ_REL);
    unsigned tail = __atomic_load_n(&state->tx_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&state->tx_head, __ATOMIC_ACQUIRE);
    while (tail != head && !state->tx_queue[tail].length) {
        tail = (tail + 1U) % LUNA_NET_TX_QUEUE_COUNT;
    }
    __atomic_store_n(&state->tx_tail, tail, __ATOMIC_RELEASE);
    seL4_Signal(state->tx_ntfn.cptr);
    if (tail == head &&
        !__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE))
        seL4_Signal(state->tx_idle_ntfn.cptr);
}

static void process_tx_queue(struct luna_network_state *state)
{
    if (__atomic_load_n(&state->tx_stress_gate, __ATOMIC_ACQUIRE))
        return;
    for (;;) {
        unsigned submit = __atomic_load_n(&state->tx_submit,
                                           __ATOMIC_RELAXED);
        unsigned head = __atomic_load_n(&state->tx_head, __ATOMIC_ACQUIRE);
        if (submit == head) return;
        struct luna_dma_buffer *buffer = NULL;
        for (unsigned i = 0; i < LUNA_NET_TX_DMA_COUNT; i++) {
            if (!state->tx_dma[i].in_use) {
                buffer = &state->tx_dma[i];
                break;
            }
        }
        if (!buffer) return;
        struct luna_tx_packet *packet = &state->tx_queue[submit];
        size_t length = packet->length;
        if (!length || length > LUNA_NET_PACKET_SIZE) return;
        memcpy(buffer->virt, packet->data, length);
        uintptr_t phys = buffer->phys;
        unsigned int packet_length = (unsigned int)length;
        buffer->queue_index = submit;
        buffer->in_use = 1;
        unsigned next = (submit + 1U) % LUNA_NET_TX_QUEUE_COUNT;
        __atomic_store_n(&state->tx_submit, next, __ATOMIC_RELEASE);
        __atomic_fetch_add(&state->tx_pending, 1U, __ATOMIC_ACQ_REL);
        int result = state->driver.i_fn.raw_tx(&state->driver, 1, &phys,
                                               &packet_length, buffer);
        if (result == ETHIF_TX_FAILED) {
            __atomic_store_n(&state->tx_submit, submit, __ATOMIC_RELEASE);
            __atomic_fetch_sub(&state->tx_pending, 1U, __ATOMIC_ACQ_REL);
            buffer->in_use = 0;
            __atomic_fetch_add(&state->tx_driver_retries, 1,
                               __ATOMIC_RELAXED);
            return;
        }
        if (result == ETHIF_TX_COMPLETE) transmit_complete(state, buffer);
    }
}

static void record_irq_packets(struct luna_network_state *state,
                               unsigned processed)
{
    __atomic_fetch_add(&state->irq_packets, processed, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(&state->irq_max_packets,
                                        __ATOMIC_RELAXED);
    while (processed > maximum &&
           !__atomic_compare_exchange_n(&state->irq_max_packets, &maximum,
                                        processed, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
}

static unsigned network_poll_budget(struct luna_network_state *state,
                                    int *more)
{
    return state->driver.i_fn.raw_poll_budget(
        &state->driver, LUNA_NET_INTX_BUDGET, more);
}

static int tx_response_pending(struct luna_network_state *state)
{
    struct luna_net_batch_header *header = state->io_mapping;
    seL4_Word request = __atomic_load_n(&header->tx_request_sequence,
                                        __ATOMIC_ACQUIRE);
    return request && request != __atomic_load_n(
        &header->tx_response_sequence, __ATOMIC_ACQUIRE);
}

static void service_tx_timeout_retry(struct luna_network_state *state)
{
    struct luna_net_batch_header *header = state->io_mapping;
    seL4_Word sequence = __atomic_load_n(&header->tx_request_sequence,
                                         __ATOMIC_ACQUIRE);
    if (!sequence || sequence == __atomic_load_n(
            &header->tx_response_sequence, __ATOMIC_ACQUIRE))
        return;
    (void)luna_timer_manager_cancel_network();
    __atomic_store_n(&header->tx_accepted, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&header->tx_response_sequence, sequence,
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&state->tx_timeout_retries, 1,
                       __ATOMIC_RELAXED);
    seL4_Signal(state->tx_ntfn.cptr);
}

static void network_irq_thread(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct luna_network_state *state = arg0;
    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(state->irq_ntfn.cptr, &badge);
        if (!__atomic_load_n(&state->irq_mode, __ATOMIC_ACQUIRE))
            continue;
        seL4_Signal(state->irq_badged_ntfn_path.capPtr);
        seL4_Wait(state->irq_done_ntfn.cptr, &badge);
        if (seL4_IRQHandler_Ack(state->irq_handler_path.capPtr) !=
            seL4_NoError) {
            __atomic_fetch_add(&state->irq_errors, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&state->irq_mode, 0, __ATOMIC_RELEASE);
            seL4_Signal(state->poll_kick_ntfn.cptr);
        }
    }
}

static void network_poll_thread(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct luna_network_state *state = arg0;
    for (;;) {
        if (__atomic_load_n(&state->irq_mode, __ATOMIC_ACQUIRE)) {
            seL4_Word badge = 0;
            seL4_Wait(state->poll_kick_ntfn.cptr, &badge);
            int irq_event = (badge & LUNA_NET_IRQ_BADGE) != 0;
            int timer_event = (badge & LUNA_NET_TIMER_BADGE) != 0;
            __atomic_store_n(&state->poll_in_driver, 1,
                             __ATOMIC_RELEASE);
            if (irq_event) {
                state->driver.i_fn.raw_ack_irq(&state->driver,
                                               (int)state->irq_line);
                __atomic_fetch_add(&state->irq_count, 1,
                                   __ATOMIC_RELAXED);
                seL4_Signal(state->irq_done_ntfn.cptr);
            }
            int more = 0;
            unsigned poll_index = 0;
            do {
                if (__atomic_load_n(&state->child_active,
                                    __ATOMIC_ACQUIRE) &&
                    rx_queue_full(state))
                    record_rx_backpressure(state);
                unsigned processed = network_poll_budget(state, &more);
                __atomic_fetch_add(&state->irq_kick_polls, 1,
                                   __ATOMIC_RELAXED);
                if (irq_event && !poll_index)
                    record_irq_packets(state, processed);
                else if (processed) {
                    __atomic_fetch_add(&state->irq_coalesced_polls, 1,
                                       __ATOMIC_RELAXED);
                    __atomic_fetch_add(&state->irq_coalesced_packets,
                                       processed, __ATOMIC_RELAXED);
                }
                if (more)
                    __atomic_fetch_add(&state->irq_budget_exhaustions, 1,
                                       __ATOMIC_RELAXED);
                process_tx_queue(state);
                service_tx_notification(state);
                process_tx_queue(state);
                if (tx_response_pending(state)) {
                    int recheck_more = 0;
                    unsigned rechecked = network_poll_budget(
                        state, &recheck_more);
                    __atomic_fetch_add(&state->irq_kick_polls, 1,
                                       __ATOMIC_RELAXED);
                    if (rechecked) {
                        __atomic_fetch_add(&state->irq_coalesced_polls, 1,
                                           __ATOMIC_RELAXED);
                        __atomic_fetch_add(
                            &state->irq_coalesced_packets, rechecked,
                            __ATOMIC_RELAXED);
                    }
                    if (recheck_more) {
                        __atomic_fetch_add(
                            &state->irq_budget_exhaustions, 1,
                            __ATOMIC_RELAXED);
                        more = 1;
                    }
                    process_tx_queue(state);
                    service_tx_notification(state);
                    process_tx_queue(state);
                    if (tx_response_pending(state)) {
                        if (timer_event) {
                            /* A bounded recheck still found no queue space.
                             * Return one retry after 5 ms instead of leaving
                             * the child blocked forever waiting for an INTx
                             * edge that may never recur. */
                            service_tx_timeout_retry(state);
                            timer_event = 0;
                        } else if (luna_timer_manager_schedule_network(
                                       state->timer_badged_ntfn_path.capPtr,
                                       LUNA_NET_TX_RECHECK_NS)) {
                            __atomic_fetch_add(&state->irq_errors, 1,
                                               __ATOMIC_RELAXED);
                        }
                    }
                }
                poll_index++;
            } while (more);
            __atomic_store_n(&state->poll_in_driver, 0,
                             __ATOMIC_RELEASE);
            if (!__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE) &&
                tx_queue_empty(state))
                seL4_Signal(state->tx_idle_ntfn.cptr);
            continue;
        }
        if (!__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE) &&
            tx_queue_empty(state)) {
            seL4_Word badge = 0;
            seL4_Wait(state->poll_kick_ntfn.cptr, &badge);
            continue;
        }
        __atomic_store_n(&state->poll_in_driver, 1, __ATOMIC_RELEASE);
        process_tx_queue(state);
        service_tx_notification(state);
        process_tx_queue(state);
        int child_active = __atomic_load_n(&state->child_active,
                                            __ATOMIC_ACQUIRE);
        int tx_pending = __atomic_load_n(&state->tx_pending,
                                          __ATOMIC_ACQUIRE);
        int queue_full = child_active && rx_queue_full(state);
        if (queue_full && !tx_pending) {
            record_rx_backpressure(state);
            __atomic_store_n(&state->poll_in_driver, 0,
                             __ATOMIC_RELEASE);
            seL4_Yield();
            continue;
        }
        if (child_active && !queue_full)
            __atomic_store_n(&state->backpressure_active, 0,
                             __ATOMIC_RELEASE);
        if (!child_active && !tx_pending && tx_queue_empty(state)) {
            __atomic_store_n(&state->poll_in_driver, 0,
                             __ATOMIC_RELEASE);
            seL4_Signal(state->tx_idle_ntfn.cptr);
            continue;
        }
        if (!__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&state->poll_in_driver, 0, __ATOMIC_RELEASE);
            continue;
        }
        state->driver.i_fn.raw_poll(&state->driver);
        __atomic_fetch_add(&state->fallback_polls, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&state->poll_in_driver, 0, __ATOMIC_RELEASE);
        seL4_Yield();
    }
}

static int init_shared_window(vspace_t *manager_vspace)
{
    network.io_reservation = vspace_reserve_range_at(
        manager_vspace, MANAGER_NET_IO_ADDR, LUNA_NET_IO_SIZE,
        seL4_AllRights, 1);
    if (!network.io_reservation.res) return -1;
    if (vspace_new_pages_at_vaddr(manager_vspace, MANAGER_NET_IO_ADDR,
                                  LUNA_NET_IO_PAGES, seL4_PageBits,
                                  network.io_reservation))
        return -1;
    network.io_mapping = MANAGER_NET_IO_ADDR;
    network.io_allocated = 1;
    memset(network.io_mapping, 0, LUNA_NET_IO_SIZE);
    return 0;
}

static void cleanup_network_irq(vka_t *vka, vspace_t *manager_vspace)
{
    (void)manager_vspace;
    __atomic_store_n(&network.irq_mode, 0, __ATOMIC_RELEASE);
    if (network.irq_handler_path.capPtr) {
        if (seL4_IRQHandler_Clear(network.irq_handler_path.capPtr) !=
            seL4_NoError)
            printf("luna: virtio-net IRQ clear failed\n");
    }
    if (network.irq_badged_ntfn_path.capPtr) {
        vka_cnode_delete(&network.irq_badged_ntfn_path);
        vka_cspace_free_path(vka, network.irq_badged_ntfn_path);
        memset(&network.irq_badged_ntfn_path, 0,
               sizeof(network.irq_badged_ntfn_path));
    }
    if (network.irq_handler_path.capPtr) {
        vka_cnode_delete(&network.irq_handler_path);
        vka_cspace_free_path(vka, network.irq_handler_path);
        memset(&network.irq_handler_path, 0,
               sizeof(network.irq_handler_path));
    }
    if (network.irq_done_ntfn.cptr) {
        vka_free_object(vka, &network.irq_done_ntfn);
        memset(&network.irq_done_ntfn, 0, sizeof(network.irq_done_ntfn));
    }
    if (network.irq_ntfn.cptr) {
        vka_free_object(vka, &network.irq_ntfn);
        memset(&network.irq_ntfn, 0, sizeof(network.irq_ntfn));
    }
}

static int init_network_irq(simple_t *simple, vka_t *vka,
                            vspace_t *manager_vspace)
{
    uint32_t line = 0, pin = 0;
    if (!network.driver.i_fn.raw_handleIRQ ||
        !network.driver.i_fn.raw_ack_irq ||
        !network.driver.i_fn.raw_poll_budget ||
        pci_read(PCI_INTERRUPT_LINE, 1, &line) ||
        pci_read(PCI_INTERRUPT_PIN, 1, &pin) ||
        !line || line >= 24U || pin != 1U) {
        printf("luna: virtio-net IRQ unavailable, using polling "
               "line=%u pin=%u\n", line, pin);
        return 0;
    }
    if (vka_alloc_notification(vka, &network.irq_ntfn) ||
        vka_alloc_notification(vka, &network.irq_done_ntfn) ||
        vka_cspace_alloc_path(vka, &network.irq_handler_path) ||
        arch_simple_get_ioapic(&simple->arch_simple,
                               network.irq_handler_path, 0, line, 1, 1,
                               line) != seL4_NoError ||
        vka_mint_object(vka, &network.poll_kick_ntfn,
                        &network.irq_badged_ntfn_path,
                        seL4_AllRights, LUNA_NET_IRQ_BADGE) ||
        seL4_IRQHandler_SetNotification(
            network.irq_handler_path.capPtr,
            network.irq_ntfn.cptr) != seL4_NoError) {
        printf("luna: virtio-net IRQ relay setup failed line=%u, "
               "using polling\n", line);
        cleanup_network_irq(vka, manager_vspace);
        return 0;
    }
    if (seL4_IRQHandler_Ack(network.irq_handler_path.capPtr) !=
        seL4_NoError) {
        printf("luna: virtio-net initial IRQ acknowledge failed, "
               "using polling\n");
        cleanup_network_irq(vka, manager_vspace);
        return 0;
    }
    network.irq_line = line;
    __atomic_store_n(&network.irq_mode, 1, __ATOMIC_RELEASE);
    printf("luna: manager virtio-net IRQ ready line=%u mode=intx-relay\n",
           network.irq_line);
    return 0;
}

int luna_network_manager_init(simple_t *simple, vka_t *vka,
                              vspace_t *manager_vspace)
{
    uint32_t vendor = 0, device = 0, bar0 = 0, command = 0;
    network_state = calloc(1, sizeof(*network_state));
    if (!network_state) {
        printf("luna: manager network state allocation failed\n");
        return -1;
    }
    if (sel4platsupport_new_io_ops(manager_vspace, vka, simple,
                                   &network.io_ops) ||
        sel4platsupport_new_arch_ops(&network.io_ops, simple, vka) ||
        sel4utils_new_page_dma_alloc(vka, manager_vspace,
                                     &network.io_ops.dma_manager)) {
        printf("luna: manager network I/O setup failed\n");
        return -1;
    }
    if (pci_read(0x00, 2, &vendor) || pci_read(0x02, 2, &device) ||
        vendor != LUNA_NET_PCI_VENDOR || device != LUNA_NET_PCI_DEVICE_ID) {
        printf("luna: virtio-net PCI device missing at 00:05.0 "
               "vendor=%04x device=%04x\n", vendor, device);
        return -1;
    }
    if (pci_read(PCI_BAR0, 4, &bar0) || !(bar0 & 1U) ||
        !(bar0 & ~3U) || pci_read(PCI_COMMAND, 2, &command) ||
        pci_write(PCI_COMMAND, 2,
                  command | PCI_COMMAND_IO | PCI_COMMAND_MASTER)) {
        printf("luna: virtio-net PCI configuration invalid bar0=%08x\n",
               bar0);
        return -1;
    }
    for (unsigned i = 0; i < LUNA_NET_TX_DMA_COUNT; i++) {
        dma_addr_t tx = dma_alloc_pin(&network.io_ops.dma_manager,
                                      LUNA_NET_PACKET_SIZE, 1, 16);
        if (!tx.virt || !tx.phys) {
            printf("luna: manager network TX DMA allocation failed "
                   "slot=%u\n", i);
            return -1;
        }
        network.tx_dma[i].virt = tx.virt;
        network.tx_dma[i].phys = tx.phys;
    }
    if (init_shared_window(manager_vspace)) {
        printf("luna: manager network window allocation failed\n");
        return -1;
    }
    network.mac[0] = 0x52;
    network.mac[1] = 0x54;
    network.mac[2] = 0x00;
    network.mac[3] = 0x12;
    network.mac[4] = 0x34;
    network.mac[5] = 0x56;
    network.driver.i_cb.allocate_rx_buf = allocate_rx_buffer;
    network.driver.i_cb.rx_complete = receive_complete;
    network.driver.i_cb.tx_complete = transmit_complete;
    network.driver.cb_cookie = &network;
    ethif_virtio_pci_config_t config = {
        .io_base = (uint16_t)(bar0 & ~3U),
        .mmio_base = NULL,
    };
    if (ethif_virtio_pci_init(&network.driver, network.io_ops, &config)) {
        printf("luna: manager virtio-net driver init failed io=%04x\n",
               config.io_base);
        return -1;
    }
    int mtu = 0;
    network.driver.i_fn.low_level_init(&network.driver, network.mac, &mtu);
    if (mtu <= 0 || mtu + 64 > (int)LUNA_NET_PACKET_SIZE) {
        printf("luna: manager virtio-net MTU invalid: %d\n", mtu);
        return -1;
    }
    if (vka_alloc_notification(vka, &network.rx_ntfn) ||
        vka_alloc_notification(vka, &network.tx_ntfn) ||
        vka_alloc_notification(vka, &network.tx_idle_ntfn) ||
        vka_alloc_notification(vka, &network.poll_kick_ntfn) ||
        vka_mint_object(vka, &network.poll_kick_ntfn,
                        &network.timer_badged_ntfn_path,
                        seL4_AllRights, LUNA_NET_TIMER_BADGE)) {
        printf("luna: manager network notification allocation failed\n");
        return -1;
    }
    if (init_network_irq(simple, vka, manager_vspace)) return -1;
    sel4utils_thread_config_t thread_config = thread_config_new(simple);
    thread_config = thread_config_priority(thread_config,
                                           LUNA_NET_POLL_PRIORITY);
    thread_config = thread_config_stack_size(thread_config,
                                              LUNA_NET_POLL_STACK_PAGES);
    thread_config = thread_config_create_reply(thread_config);
    if (sel4utils_configure_thread_config(vka, manager_vspace,
                                          manager_vspace, thread_config,
                                          &network.poll_thread) ||
        sel4utils_start_thread(&network.poll_thread, network_poll_thread,
                               &network, NULL, 1)) {
        printf("luna: manager network poll thread setup failed\n");
        return -1;
    }
    NAME_THREAD(network.poll_thread.tcb.cptr, "luna-net-rx");
    if (__atomic_load_n(&network.irq_mode, __ATOMIC_ACQUIRE)) {
        thread_config = thread_config_new(simple);
        thread_config = thread_config_priority(thread_config,
                                               LUNA_NET_IRQ_PRIORITY);
        thread_config = thread_config_stack_size(thread_config,
                                                  LUNA_NET_IRQ_STACK_PAGES);
        thread_config = thread_config_create_reply(thread_config);
        if (sel4utils_configure_thread_config(vka, manager_vspace,
                                              manager_vspace, thread_config,
                                              &network.irq_thread) ||
            sel4utils_start_thread(&network.irq_thread, network_irq_thread,
                                   &network, NULL, 1)) {
            printf("luna: manager network IRQ thread setup failed\n");
            return -1;
        }
        NAME_THREAD(network.irq_thread.tcb.cptr, "luna-net-irq");
    }
    network.ready = 1;
    printf("luna: manager virtio-net ready PCI=00:05.0 io=%04x "
           "mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           config.io_base, network.mac[0], network.mac[1], network.mac[2],
           network.mac[3], network.mac[4], network.mac[5]);
    return 0;
}

int luna_network_map_child(vka_t *vka, vspace_t *manager_vspace,
                           sel4utils_process_t *process,
                           struct luna_net_mapping *mapping,
                           int activate)
{
    if (!network.ready || !network.io_allocated) return -1;
    mapping->reservation = vspace_reserve_range_at(
        &process->vspace, (void *)(uintptr_t)LUNA_NET_IO_BASE,
        LUNA_NET_IO_SIZE, seL4_AllRights, 1);
    if (!mapping->reservation.res) return -1;
    mapping->reserved = 1;
    for (size_t i = 0; i < LUNA_NET_IO_PAGES; i++) {
        void *source = (void *)((uintptr_t)network.io_mapping +
                               i * BIT(seL4_PageBits));
        seL4_CPtr source_cap = vspace_get_cap(manager_vspace, source);
        if (!source_cap || vka_cspace_alloc_path(vka,
                                                 &mapping->frame_caps[i]))
            return -1;
        cspacepath_t source_path;
        vka_cspace_make_path(vka, source_cap, &source_path);
        if (vka_cnode_copy(&mapping->frame_caps[i], &source_path,
                           seL4_AllRights)) {
            vka_cspace_free_path(vka, mapping->frame_caps[i]);
            return -1;
        }
        mapping->cap_allocated[i] = 1;
        seL4_CPtr cap = mapping->frame_caps[i].capPtr;
        void *child_address = (void *)(uintptr_t)(
            LUNA_NET_IO_BASE + i * BIT(seL4_PageBits));
        if (vspace_map_pages_at_vaddr(&process->vspace, &cap, NULL,
                                      child_address, 1, seL4_PageBits,
                                      mapping->reservation))
            return -1;
        mapping->mapped_pages++;
    }
    cspacepath_t notification_path;
    vka_cspace_make_path(vka, network.rx_ntfn.cptr, &notification_path);
    mapping->child_rx_ntfn = sel4utils_mint_cap_to_process(
        process, notification_path,
        seL4_CapRights_new(false, false, true, false), 0);
    if (!mapping->child_rx_ntfn) return -1;
    vka_cspace_make_path(vka, network.tx_ntfn.cptr, &notification_path);
    mapping->child_tx_ntfn = sel4utils_mint_cap_to_process(
        process, notification_path,
        seL4_CapRights_new(false, false, true, false), 0);
    if (!mapping->child_tx_ntfn) return -1;
    vka_cspace_make_path(vka, network.poll_kick_ntfn.cptr,
                         &notification_path);
    mapping->child_tx_submit_ntfn = sel4utils_mint_cap_to_process(
        process, notification_path,
        seL4_CapRights_new(false, false, false, true),
        LUNA_NET_TX_SUBMIT_BADGE);
    if (!mapping->child_tx_submit_ntfn) return -1;

    seL4_Word badge = 0;
    seL4_Poll(network.rx_ntfn.cptr, &badge);
    seL4_Poll(network.tx_ntfn.cptr, &badge);
    seL4_Poll(network.tx_idle_ntfn.cptr, &badge);
    __atomic_store_n(&network.rx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_submit, 0, __ATOMIC_RELEASE);
    struct luna_net_batch_header *header = network.io_mapping;
    __atomic_store_n(&header->tx_request_sequence, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->tx_response_sequence, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->tx_accepted, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->rx_produced, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->rx_consumed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&header->rx_waiting, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.notification_masked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.backpressure_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_drops, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_high_water, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_backpressure, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_empty_fetches, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_high_water, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_backpressure, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_driver_retries, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_completed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_endpoint_requests, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_endpoint_batches, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_endpoint_packets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_shared_copies, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_kicks, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_timeout_retries, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_endpoint_requests, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_endpoint_batches, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_endpoint_packets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_shared_copies, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_stress_gate, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_packets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_coalesced_polls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_coalesced_packets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_budget_exhaustions, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_max_packets, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_kick_polls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.fallback_polls, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.irq_errors, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_pending, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < LUNA_NET_TX_DMA_COUNT; i++) {
        network.tx_dma[i].in_use = 0;
        network.tx_dma[i].queue_index = 0;
    }
    __atomic_store_n(&network.child_active, activate != 0,
                     __ATOMIC_RELEASE);
    if (activate) seL4_Signal(network.poll_kick_ntfn.cptr);
    return 0;
}

void luna_network_deactivate_child(void)
{
    if (!network_state || network_state == (void *)(uintptr_t)1 ||
        !network.ready)
        return;
    __atomic_store_n(&network.child_active, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_stress_gate, 0, __ATOMIC_RELEASE);
    seL4_Signal(network.poll_kick_ntfn.cptr);
    while (__atomic_load_n(&network.poll_in_driver, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&network.tx_pending, __ATOMIC_ACQUIRE) ||
           !tx_queue_empty(&network)) {
        seL4_Word idle_badge = 0;
        seL4_Wait(network.tx_idle_ntfn.cptr, &idle_badge);
    }
    /* The poll worker can arm the bounded pending-response recheck just
     * before observing child_active == 0.  Cancel it after the worker is
     * quiescent so a stale wake cannot leak into the next child instance. */
    (void)luna_timer_manager_cancel_network();
    __atomic_store_n(&network.notification_masked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.backpressure_active, 0, __ATOMIC_RELEASE);
    seL4_Word badge = 0;
    seL4_Poll(network.rx_ntfn.cptr, &badge);
    seL4_Signal(network.tx_ntfn.cptr);
}

int luna_network_verify_irq(void)
{
    uint64_t interrupts = __atomic_load_n(&network.irq_count,
                                           __ATOMIC_ACQUIRE);
    uint64_t kick_polls = __atomic_load_n(&network.irq_kick_polls,
                                           __ATOMIC_ACQUIRE);
    uint64_t fallback_polls = __atomic_load_n(&network.fallback_polls,
                                               __ATOMIC_ACQUIRE);
    uint64_t errors = __atomic_load_n(&network.irq_errors,
                                       __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&network.irq_mode, __ATOMIC_ACQUIRE)) {
        if (!interrupts || fallback_polls || errors) return -1;
        printf("LUNA_NETWORK_IRQ_OK line=%u interrupts=%llu "
               "kick_polls=%llu fallback_polls=%llu\n",
               network.irq_line, (unsigned long long)interrupts,
               (unsigned long long)kick_polls,
               (unsigned long long)fallback_polls);
        uint64_t irq_packets = __atomic_load_n(&network.irq_packets,
                                               __ATOMIC_ACQUIRE);
        uint64_t coalesced_polls = __atomic_load_n(
            &network.irq_coalesced_polls, __ATOMIC_ACQUIRE);
        uint64_t coalesced_packets = __atomic_load_n(
            &network.irq_coalesced_packets, __ATOMIC_ACQUIRE);
        uint64_t exhaustions = __atomic_load_n(
            &network.irq_budget_exhaustions, __ATOMIC_ACQUIRE);
        uint64_t maximum = __atomic_load_n(&network.irq_max_packets,
                                           __ATOMIC_ACQUIRE);
        if (maximum > LUNA_NET_INTX_BUDGET) return -1;
        printf("LUNA_NETWORK_IRQ_BUDGET_OK budget=%u irq_packets=%llu "
               "coalesced_polls=%llu coalesced_packets=%llu "
               "budget_exhaustions=%llu max_packets_per_irq=%llu "
               "packets_per_irq_milli=%llu\n",
               LUNA_NET_INTX_BUDGET,
               (unsigned long long)irq_packets,
               (unsigned long long)coalesced_polls,
               (unsigned long long)coalesced_packets,
               (unsigned long long)exhaustions,
               (unsigned long long)maximum,
               (unsigned long long)(irq_packets * 1000ULL / interrupts));
        printf("LUNA_NET_MANAGER_COUNTERS tx_ipc=%llu tx_batches=%llu "
               "tx_packets=%llu tx_copies=%llu tx_kicks=%llu "
               "rx_ipc=%llu rx_batches=%llu rx_packets=%llu "
               "rx_copies=%llu\n",
               (unsigned long long)__atomic_load_n(
                   &network.tx_endpoint_requests, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.tx_endpoint_batches, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.tx_endpoint_packets, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.tx_shared_copies, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.tx_kicks, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.rx_endpoint_requests, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.rx_endpoint_batches, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.rx_endpoint_packets, __ATOMIC_ACQUIRE),
               (unsigned long long)__atomic_load_n(
                   &network.rx_shared_copies, __ATOMIC_ACQUIRE));
        printf("LUNA_NET_TX_RECOVERY_STATS timeout_retries=%llu\n",
               (unsigned long long)__atomic_load_n(
                   &network.tx_timeout_retries, __ATOMIC_ACQUIRE));
        return 0;
    }
    if (!fallback_polls) return -1;
    printf("LUNA_NETWORK_IRQ_FALLBACK_OK polls=%llu errors=%llu\n",
           (unsigned long long)fallback_polls,
           (unsigned long long)errors);
    return 0;
}

static int service_tx_batch(size_t requested, seL4_Word *response)
{
    struct luna_net_batch_header *header = network.io_mapping;
    __atomic_fetch_add(&network.tx_endpoint_requests, 1,
                       __ATOMIC_RELAXED);
    if (!requested || requested > LUNA_NET_BATCH_SLOTS ||
        header->tx_count != requested ||
        !__atomic_load_n(&network.child_active, __ATOMIC_ACQUIRE))
        return -1;
    unsigned head = __atomic_load_n(&network.tx_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&network.tx_tail, __ATOMIC_ACQUIRE);
    unsigned used = (head + LUNA_NET_TX_QUEUE_COUNT - tail) %
                    LUNA_NET_TX_QUEUE_COUNT;
    unsigned available = LUNA_NET_TX_QUEUE_COUNT - 1U - used;
    unsigned accepted = requested < available ? (unsigned)requested :
                        available;
    if (!accepted) {
        __atomic_fetch_add(&network.tx_backpressure, 1, __ATOMIC_RELAXED);
        if (__atomic_exchange_n(&network.tx_stress_gate, 0,
                                __ATOMIC_ACQ_REL)) {
            seL4_Signal(network.poll_kick_ntfn.cptr);
            __atomic_fetch_add(&network.tx_kicks, 1, __ATOMIC_RELAXED);
        }
        *response = 0;
        return 0;
    }
    for (unsigned i = 0; i < accepted; i++) {
        size_t length = (size_t)header->tx_lengths[i];
        if (!length || length > LUNA_NET_PACKET_SIZE) return -1;
    }
    int was_empty = head == tail;
    for (unsigned i = 0; i < accepted; i++) {
        size_t length = (size_t)header->tx_lengths[i];
        struct luna_tx_packet *packet = &network.tx_queue[head];
        memcpy(packet->data,
               (unsigned char *)network.io_mapping + LUNA_NET_TX_OFFSET +
                   i * LUNA_NET_PACKET_SIZE,
               length);
        packet->length = length;
        head = (head + 1U) % LUNA_NET_TX_QUEUE_COUNT;
    }
    __atomic_store_n(&network.tx_head, head, __ATOMIC_RELEASE);
    used += accepted;
    uint64_t high_water = __atomic_load_n(&network.tx_high_water,
                                           __ATOMIC_RELAXED);
    while (used > high_water &&
           !__atomic_compare_exchange_n(&network.tx_high_water,
                                        &high_water, used, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
    __atomic_fetch_add(&network.tx_endpoint_batches, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&network.tx_endpoint_packets, accepted,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&network.tx_shared_copies, accepted,
                       __ATOMIC_RELAXED);
    if (was_empty) {
        seL4_Signal(network.poll_kick_ntfn.cptr);
        __atomic_fetch_add(&network.tx_kicks, 1, __ATOMIC_RELAXED);
    }
    *response = accepted;
    return 0;
}

static void service_tx_notification(struct luna_network_state *state)
{
    if (!__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE))
        return;
    struct luna_net_batch_header *header = state->io_mapping;
    seL4_Word sequence = __atomic_load_n(&header->tx_request_sequence,
                                         __ATOMIC_ACQUIRE);
    if (!sequence || sequence == __atomic_load_n(
            &header->tx_response_sequence, __ATOMIC_ACQUIRE))
        return;
    seL4_Word accepted = 0;
    int result = service_tx_batch((size_t)header->tx_count, &accepted);
    if (!result && !accepted)
        return;
    if (result)
        accepted = ~(seL4_Word)0;
    (void)luna_timer_manager_cancel_network();
    __atomic_store_n(&header->tx_accepted, accepted, __ATOMIC_RELAXED);
    __atomic_store_n(&header->tx_response_sequence, sequence,
                     __ATOMIC_RELEASE);
    seL4_Signal(state->tx_ntfn.cptr);
}

static int service_tx(size_t length)
{
    struct luna_net_batch_header *header = network.io_mapping;
    if (!length || length > LUNA_NET_PACKET_SIZE) return -1;
    header->tx_count = 1;
    header->tx_lengths[0] = length;
    seL4_Word accepted = 0;
    int result = service_tx_batch(1, &accepted);
    return result || !accepted ? LUNA_NET_TX_RETRY : 0;
}

static int service_rx_batch(size_t requested, seL4_Word *response)
{
    __atomic_fetch_add(&network.rx_endpoint_requests, 1,
                       __ATOMIC_RELAXED);
    if (!requested || requested > LUNA_NET_BATCH_SLOTS) return -1;
    struct luna_net_batch_header *header = network.io_mapping;
    unsigned tail = __atomic_load_n(&network.rx_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&network.rx_head, __ATOMIC_ACQUIRE);
    if (tail == head) {
        __atomic_fetch_add(&network.rx_empty_fetches, 1, __ATOMIC_RELAXED);
        header->rx_count = 0;
        *response = 0;
        return 0;
    }
    unsigned count = 0;
    while (tail != head && count < requested) {
        struct luna_rx_packet *packet = &network.rx_queue[tail];
        if (!packet->length || packet->length > LUNA_NET_PACKET_SIZE)
            return -1;
        memcpy((unsigned char *)network.io_mapping + LUNA_NET_RX_OFFSET +
                   count * LUNA_NET_PACKET_SIZE,
               packet->data, packet->length);
        header->rx_lengths[count] = packet->length;
        packet->length = 0;
        tail = (tail + 1U) % LUNA_NET_RX_QUEUE_COUNT;
        count++;
    }
    header->rx_count = count;
    *response = count;
    __atomic_store_n(&network.rx_tail, tail, __ATOMIC_RELEASE);
    __atomic_fetch_add(&header->rx_consumed, count, __ATOMIC_RELEASE);
    __atomic_store_n(&network.backpressure_active, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&network.rx_endpoint_batches, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&network.rx_endpoint_packets, count,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&network.rx_shared_copies, count, __ATOMIC_RELAXED);
    return 0;
}

static int service_rx(seL4_Word *response)
{
    seL4_Word packets = 0;
    int result = service_rx_batch(1, &packets);
    if (result || !packets) {
        *response = 0;
        return result;
    }
    struct luna_net_batch_header *header = network.io_mapping;
    *response = header->rx_lengths[0];
    return 0;
}

static seL4_Word service_stats(void)
{
    uint64_t high_water_value = __atomic_load_n(&network.rx_high_water,
                                                 __ATOMIC_ACQUIRE);
    uint64_t backpressure_value = __atomic_load_n(&network.rx_backpressure,
                                                   __ATOMIC_ACQUIRE);
    uint64_t drops_value = __atomic_load_n(&network.rx_drops,
                                            __ATOMIC_ACQUIRE);
    uint64_t empty_value = __atomic_load_n(&network.rx_empty_fetches,
                                            __ATOMIC_ACQUIRE);
    return luna_net_stats_pack(high_water_value, backpressure_value,
                               drops_value, empty_value);
}

static seL4_Word service_tx_stats(void)
{
    uint64_t high_water = __atomic_load_n(&network.tx_high_water,
                                           __ATOMIC_ACQUIRE);
    uint64_t backpressure = __atomic_load_n(&network.tx_backpressure,
                                             __ATOMIC_ACQUIRE);
    uint64_t retries = __atomic_load_n(&network.tx_driver_retries,
                                        __ATOMIC_ACQUIRE);
    uint64_t completed = __atomic_load_n(&network.tx_completed,
                                          __ATOMIC_ACQUIRE);
    return luna_net_stats_pack(high_water, backpressure, retries, completed);
}

static void service_debug(void)
{
    unsigned dma_in_use = 0;
    for (unsigned i = 0; i < LUNA_NET_RX_DMA_COUNT; i++)
        dma_in_use += network.rx_dma[i].in_use != 0;
    unsigned head = __atomic_load_n(&network.rx_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(&network.rx_tail, __ATOMIC_ACQUIRE);
    struct luna_net_batch_header *header = network.io_mapping;
    printf("LUNA_NET_MANAGER_DEBUG rx_head=%u rx_tail=%u rx_used=%u "
           "rx_dma_in_use=%u rx_produced=%lu rx_consumed=%lu "
           "rx_waiting=%lu masked=%d "
           "irq_count=%llu kick_polls=%llu "
           "budget_exhaustions=%llu irq_errors=%llu\n",
           head, tail,
           (head + LUNA_NET_RX_QUEUE_COUNT - tail) %
               LUNA_NET_RX_QUEUE_COUNT,
           dma_in_use,
           (unsigned long)__atomic_load_n(&header->rx_produced,
                                           __ATOMIC_ACQUIRE),
           (unsigned long)__atomic_load_n(&header->rx_consumed,
                                           __ATOMIC_ACQUIRE),
           (unsigned long)__atomic_load_n(&header->rx_waiting,
                                           __ATOMIC_ACQUIRE),
           __atomic_load_n(&network.notification_masked, __ATOMIC_ACQUIRE),
           (unsigned long long)__atomic_load_n(&network.irq_count,
                                                __ATOMIC_ACQUIRE),
           (unsigned long long)__atomic_load_n(&network.irq_kick_polls,
                                                __ATOMIC_ACQUIRE),
           (unsigned long long)__atomic_load_n(
               &network.irq_budget_exhaustions, __ATOMIC_ACQUIRE),
           (unsigned long long)__atomic_load_n(&network.irq_errors,
                                                __ATOMIC_ACQUIRE));
    network.driver.i_fn.print_state(&network.driver);
}

static int service_control(seL4_Word control)
{
    if (control > 1) return -1;
    __atomic_store_n(&network.notification_masked, control != 0,
                     __ATOMIC_RELEASE);
    if (!control &&
        __atomic_load_n(&network.rx_head, __ATOMIC_ACQUIRE) !=
        __atomic_load_n(&network.rx_tail, __ATOMIC_ACQUIRE))
        signal_rx(&network);
    return 0;
}

static int service_tx_stress(seL4_Word control)
{
    if (control > 1) return -1;
    if (control) {
        while (!tx_queue_empty(&network) ||
               __atomic_load_n(&network.tx_pending, __ATOMIC_ACQUIRE) ||
               __atomic_load_n(&network.poll_in_driver, __ATOMIC_ACQUIRE)) {
            seL4_Signal(network.poll_kick_ntfn.cptr);
            seL4_Word badge = 0;
            seL4_Wait(network.tx_idle_ntfn.cptr, &badge);
        }
    }
    __atomic_store_n(&network.tx_stress_gate, control != 0,
                     __ATOMIC_RELEASE);
    if (!control) {
        seL4_Signal(network.poll_kick_ntfn.cptr);
        seL4_Signal(network.tx_ntfn.cptr);
    }
    return 0;
}

int luna_network_service(seL4_CPtr command_ep, seL4_Word event,
                         seL4_Word length_word)
{
    seL4_Word response = 0;
    int error = -1;
    if (network.ready) {
        if (event == LUNA_ISOLATION_EVENT_NET_TX)
            error = service_tx((size_t)length_word);
        else if (event == LUNA_ISOLATION_EVENT_NET_TX_BATCH)
            error = service_tx_batch((size_t)length_word, &response);
        else if (event == LUNA_ISOLATION_EVENT_NET_RX && !length_word)
            error = service_rx(&response);
        else if (event == LUNA_ISOLATION_EVENT_NET_RX_BATCH)
            error = service_rx_batch((size_t)length_word, &response);
        else if (event == LUNA_ISOLATION_EVENT_NET_WAKE && !length_word) {
            seL4_Signal(network.rx_ntfn.cptr);
            error = 0;
        } else if (event == LUNA_ISOLATION_EVENT_NET_CONTROL)
            error = service_control(length_word);
        else if (event == LUNA_ISOLATION_EVENT_NET_STATS && !length_word) {
            response = service_stats();
            error = 0;
        } else if (event == LUNA_ISOLATION_EVENT_NET_TX_STATS &&
                   !length_word) {
            response = service_tx_stats();
            error = 0;
        } else if (event == LUNA_ISOLATION_EVENT_NET_TX_STRESS)
            error = service_tx_stress(length_word);
        else if (event == LUNA_ISOLATION_EVENT_NET_DEBUG && !length_word) {
            service_debug();
            error = 0;
        }
    }
    __sync_synchronize();
    seL4_SetMR(0, LUNA_COMMAND_NET_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_SetMR(2, response);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    if (error && error != LUNA_NET_TX_RETRY)
        printf("luna: child network request failed event=%lu length=%lu\n",
               event, length_word);
    return error == LUNA_NET_TX_RETRY ? 0 : error;
}
