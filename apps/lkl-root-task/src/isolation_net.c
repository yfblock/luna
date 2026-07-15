/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Manager-owned QEMU virtio-net backend. The child only receives two bounded
 * packet pages; PCI I/O authority and all DMA memory stay in the root manager.
 */
#include "luna_network_manager.h"

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
#define LUNA_NET_TX_QUEUE_COUNT 16U
#define LUNA_NET_POLL_PRIORITY 100U
#define LUNA_NET_POLL_STACK_PAGES 8U
#define PCI_CONFIG_ADDRESS 0x0cf8U
#define PCI_CONFIG_DATA 0x0cfcU
#define PCI_COMMAND 0x04U
#define PCI_BAR0 0x10U
#define PCI_COMMAND_IO 0x1U
#define PCI_COMMAND_MASTER 0x4U

struct luna_dma_buffer {
    void *virt;
    uintptr_t phys;
    int in_use;
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
    struct luna_dma_buffer tx_dma;
    struct luna_rx_packet rx_queue[LUNA_NET_RX_QUEUE_COUNT];
    struct luna_tx_packet tx_queue[LUNA_NET_TX_QUEUE_COUNT];
    volatile unsigned rx_head;
    volatile unsigned rx_tail;
    volatile unsigned tx_head;
    volatile unsigned tx_tail;
    volatile int tx_pending;
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
    vka_object_t rx_ntfn;
    vka_object_t poll_kick_ntfn;
    sel4utils_thread_t poll_thread;
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
    if (!buffer_cookie || size > LUNA_NET_PACKET_SIZE) return 0;
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
        int was_empty = head == tail;
        struct luna_rx_packet *packet = &state->rx_queue[head];
        size_t offset = 0;
        for (unsigned i = 0; i < num_buffers; i++) {
            struct luna_dma_buffer *buffer = cookies[i];
            memcpy(packet->data + offset, buffer->virt, lengths[i]);
            offset += lengths[i];
        }
        packet->length = total;
        __atomic_store_n(&state->rx_head, next, __ATOMIC_RELEASE);
        unsigned used = (next + LUNA_NET_RX_QUEUE_COUNT - tail) %
                        LUNA_NET_RX_QUEUE_COUNT;
        uint64_t high_water = __atomic_load_n(&state->rx_high_water,
                                               __ATOMIC_RELAXED);
        while (used > high_water &&
               !__atomic_compare_exchange_n(&state->rx_high_water,
                                            &high_water, used, false,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) { }
        if (was_empty) signal_rx(state);
    } else if (total && total <= LUNA_NET_PACKET_SIZE &&
               __atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE)) {
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
    if (buffer_cookie == &state->tx_dma) {
        unsigned tail = __atomic_load_n(&state->tx_tail, __ATOMIC_RELAXED);
        state->tx_queue[tail].length = 0;
        tail = (tail + 1U) % LUNA_NET_TX_QUEUE_COUNT;
        __atomic_store_n(&state->tx_tail, tail, __ATOMIC_RELEASE);
        __atomic_fetch_add(&state->tx_completed, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&state->tx_pending, 0, __ATOMIC_RELEASE);
    }
}

static void process_tx_queue(struct luna_network_state *state)
{
    if (__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE))
        return;
    unsigned tail = __atomic_load_n(&state->tx_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&state->tx_head, __ATOMIC_ACQUIRE);
    if (tail == head) return;
    struct luna_tx_packet *packet = &state->tx_queue[tail];
    size_t length = packet->length;
    if (!length || length > LUNA_NET_PACKET_SIZE) return;
    memcpy(state->tx_dma.virt, packet->data, length);
    uintptr_t phys = state->tx_dma.phys;
    unsigned int packet_length = (unsigned int)length;
    __atomic_store_n(&state->tx_pending, 1, __ATOMIC_RELEASE);
    int result = state->driver.i_fn.raw_tx(&state->driver, 1, &phys,
                                           &packet_length,
                                           &state->tx_dma);
    if (result == ETHIF_TX_FAILED) {
        __atomic_store_n(&state->tx_pending, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&state->tx_driver_retries, 1,
                           __ATOMIC_RELAXED);
    } else if (result == ETHIF_TX_COMPLETE) {
        transmit_complete(state, &state->tx_dma);
    }
}

static void network_poll_thread(void *arg0, void *arg1, void *ipc_buffer)
{
    (void)arg1;
    (void)ipc_buffer;
    struct luna_network_state *state = arg0;
    for (;;) {
        if (!__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE) &&
            tx_queue_empty(state)) {
            seL4_Word badge = 0;
            seL4_Wait(state->poll_kick_ntfn.cptr, &badge);
            continue;
        }
        process_tx_queue(state);
        int child_active = __atomic_load_n(&state->child_active,
                                            __ATOMIC_ACQUIRE);
        int tx_pending = __atomic_load_n(&state->tx_pending,
                                          __ATOMIC_ACQUIRE);
        int queue_full = child_active && rx_queue_full(state);
        if (queue_full && !tx_pending) {
            int expected = 0;
            if (__atomic_compare_exchange_n(&state->backpressure_active,
                                            &expected, 1, false,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
                __atomic_fetch_add(&state->rx_backpressure, 1,
                                   __ATOMIC_RELAXED);
            seL4_Yield();
            continue;
        }
        if (child_active && !queue_full)
            __atomic_store_n(&state->backpressure_active, 0,
                             __ATOMIC_RELEASE);
        if (!child_active && !tx_pending && tx_queue_empty(state)) {
            continue;
        }
        __atomic_store_n(&state->poll_in_driver, 1, __ATOMIC_RELEASE);
        if (!__atomic_load_n(&state->child_active, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&state->tx_pending, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&state->poll_in_driver, 0, __ATOMIC_RELEASE);
            continue;
        }
        state->driver.i_fn.raw_poll(&state->driver);
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
    dma_addr_t tx = dma_alloc_pin(&network.io_ops.dma_manager,
                                  LUNA_NET_PACKET_SIZE, 1, 16);
    if (!tx.virt || !tx.phys || init_shared_window(manager_vspace)) {
        printf("luna: manager network DMA/window allocation failed\n");
        return -1;
    }
    network.tx_dma.virt = tx.virt;
    network.tx_dma.phys = tx.phys;
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
        vka_alloc_notification(vka, &network.poll_kick_ntfn)) {
        printf("luna: manager network notification allocation failed\n");
        return -1;
    }
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

    seL4_Word badge = 0;
    seL4_Poll(network.rx_ntfn.cptr, &badge);
    __atomic_store_n(&network.rx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.tx_tail, 0, __ATOMIC_RELEASE);
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
    __atomic_store_n(&network.tx_pending, 0, __ATOMIC_RELEASE);
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
    seL4_Signal(network.poll_kick_ntfn.cptr);
    while (__atomic_load_n(&network.poll_in_driver, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&network.tx_pending, __ATOMIC_ACQUIRE) ||
           !tx_queue_empty(&network))
        seL4_Yield();
    __atomic_store_n(&network.notification_masked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_head, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.rx_tail, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&network.backpressure_active, 0, __ATOMIC_RELEASE);
    seL4_Word badge = 0;
    seL4_Poll(network.rx_ntfn.cptr, &badge);
}

static int service_tx(size_t length)
{
    if (!length || length > LUNA_NET_PACKET_SIZE ||
        !__atomic_load_n(&network.child_active, __ATOMIC_ACQUIRE))
        return -1;
    unsigned head = __atomic_load_n(&network.tx_head, __ATOMIC_RELAXED);
    unsigned tail = __atomic_load_n(&network.tx_tail, __ATOMIC_ACQUIRE);
    unsigned next = (head + 1U) % LUNA_NET_TX_QUEUE_COUNT;
    if (next == tail) {
        __atomic_fetch_add(&network.tx_backpressure, 1, __ATOMIC_RELAXED);
        return LUNA_NET_TX_RETRY;
    }
    struct luna_tx_packet *packet = &network.tx_queue[head];
    memcpy(packet->data,
           (unsigned char *)network.io_mapping + LUNA_NET_TX_OFFSET,
           length);
    packet->length = length;
    __atomic_store_n(&network.tx_head, next, __ATOMIC_RELEASE);
    unsigned used = (next + LUNA_NET_TX_QUEUE_COUNT - tail) %
                    LUNA_NET_TX_QUEUE_COUNT;
    uint64_t high_water = __atomic_load_n(&network.tx_high_water,
                                           __ATOMIC_RELAXED);
    while (used > high_water &&
           !__atomic_compare_exchange_n(&network.tx_high_water,
                                        &high_water, used, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) { }
    seL4_Signal(network.poll_kick_ntfn.cptr);
    return 0;
}

static int service_rx(seL4_Word *response)
{
    unsigned tail = __atomic_load_n(&network.rx_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(&network.rx_head, __ATOMIC_ACQUIRE);
    if (tail == head) {
        __atomic_fetch_add(&network.rx_empty_fetches, 1, __ATOMIC_RELAXED);
        *response = 0;
        return 0;
    }
    struct luna_rx_packet *packet = &network.rx_queue[tail];
    if (!packet->length || packet->length > LUNA_NET_PACKET_SIZE)
        return -1;
    memcpy((unsigned char *)network.io_mapping + LUNA_NET_RX_OFFSET,
           packet->data, packet->length);
    *response = packet->length;
    packet->length = 0;
    tail = (tail + 1U) % LUNA_NET_RX_QUEUE_COUNT;
    __atomic_store_n(&network.rx_tail, tail, __ATOMIC_RELEASE);
    __atomic_store_n(&network.backpressure_active, 0, __ATOMIC_RELEASE);
    if (tail != __atomic_load_n(&network.rx_head, __ATOMIC_ACQUIRE))
        signal_rx(&network);
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

int luna_network_service(seL4_CPtr command_ep, seL4_Word event,
                         seL4_Word length_word)
{
    seL4_Word response = 0;
    int error = -1;
    if (network.ready) {
        if (event == LUNA_ISOLATION_EVENT_NET_TX)
            error = service_tx((size_t)length_word);
        else if (event == LUNA_ISOLATION_EVENT_NET_RX && !length_word)
            error = service_rx(&response);
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
