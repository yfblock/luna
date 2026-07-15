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
#include <utils/util.h>
#include <vka/capops.h>

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

struct luna_network_state {
    ps_io_ops_t io_ops;
    struct eth_driver driver;
    struct luna_dma_buffer rx_dma[LUNA_NET_RX_DMA_COUNT];
    struct luna_dma_buffer tx_dma;
    struct luna_rx_packet rx_queue[LUNA_NET_RX_QUEUE_COUNT];
    unsigned rx_head;
    unsigned rx_tail;
    volatile int tx_pending;
    unsigned char mac[6];
    void *io_mapping;
    reservation_t io_reservation;
    int io_reserved;
    int io_allocated;
    int ready;
};

/* Keep this live pointer in .data, not at the .bss/static-stack boundary. */
static struct luna_network_state *network_state =
    (struct luna_network_state *)(uintptr_t)1;
#define network (*network_state)

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
    unsigned next = (state->rx_head + 1U) % LUNA_NET_RX_QUEUE_COUNT;
    if (total && total <= LUNA_NET_PACKET_SIZE && next != state->rx_tail) {
        struct luna_rx_packet *packet = &state->rx_queue[state->rx_head];
        size_t offset = 0;
        for (unsigned i = 0; i < num_buffers; i++) {
            struct luna_dma_buffer *buffer = cookies[i];
            memcpy(packet->data + offset, buffer->virt, lengths[i]);
            offset += lengths[i];
        }
        packet->length = total;
        state->rx_head = next;
    }
    for (unsigned i = 0; i < num_buffers; i++) {
        struct luna_dma_buffer *buffer = cookies[i];
        if (buffer) buffer->in_use = 0;
    }
}

static void transmit_complete(void *cookie, void *buffer_cookie)
{
    struct luna_network_state *state = cookie;
    if (buffer_cookie == &state->tx_dma)
        __atomic_store_n(&state->tx_pending, 0, __ATOMIC_RELEASE);
}

static int init_shared_window(vka_t *vka, vspace_t *manager_vspace)
{
    network.io_reservation = vspace_reserve_range_at(
        manager_vspace, MANAGER_NET_IO_ADDR, LUNA_NET_IO_SIZE,
        seL4_AllRights, 1);
    if (!network.io_reservation.res) return -1;
    network.io_reserved = 1;
    if (vspace_new_pages_at_vaddr(manager_vspace, MANAGER_NET_IO_ADDR,
                                  LUNA_NET_IO_PAGES, seL4_PageBits,
                                  network.io_reservation))
        return -1;
    (void)vka;
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
    if (!tx.virt || !tx.phys || init_shared_window(vka, manager_vspace)) {
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
    network.ready = 1;
    printf("luna: manager virtio-net ready PCI=00:05.0 io=%04x "
           "mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           config.io_base, network.mac[0], network.mac[1], network.mac[2],
           network.mac[3], network.mac[4], network.mac[5]);
    return 0;
}

int luna_network_map_child(vka_t *vka, vspace_t *manager_vspace,
                           sel4utils_process_t *process,
                           struct luna_net_mapping *mapping)
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
    return 0;
}

static int service_tx(size_t length)
{
    if (!length || length > LUNA_NET_PACKET_SIZE || network.tx_pending)
        return -1;
    memcpy(network.tx_dma.virt,
           (unsigned char *)network.io_mapping + LUNA_NET_TX_OFFSET,
           length);
    uintptr_t phys = network.tx_dma.phys;
    unsigned int packet_length = (unsigned int)length;
    __atomic_store_n(&network.tx_pending, 1, __ATOMIC_RELEASE);
    int result = network.driver.i_fn.raw_tx(&network.driver, 1, &phys,
                                            &packet_length,
                                            &network.tx_dma);
    if (result == ETHIF_TX_FAILED) {
        network.tx_pending = 0;
        return -1;
    }
    if (result == ETHIF_TX_COMPLETE) network.tx_pending = 0;
    for (unsigned spin = 0; network.tx_pending && spin < 100000U; spin++) {
        network.driver.i_fn.raw_poll(&network.driver);
        seL4_Yield();
    }
    return network.tx_pending ? -1 : 0;
}

static int service_rx(seL4_Word *response)
{
    network.driver.i_fn.raw_poll(&network.driver);
    if (network.rx_tail == network.rx_head) {
        *response = 0;
        return 0;
    }
    struct luna_rx_packet *packet = &network.rx_queue[network.rx_tail];
    if (!packet->length || packet->length > LUNA_NET_PACKET_SIZE)
        return -1;
    memcpy((unsigned char *)network.io_mapping + LUNA_NET_RX_OFFSET,
           packet->data, packet->length);
    *response = packet->length;
    packet->length = 0;
    network.rx_tail = (network.rx_tail + 1U) % LUNA_NET_RX_QUEUE_COUNT;
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
    }
    __sync_synchronize();
    seL4_SetMR(0, LUNA_COMMAND_NET_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_SetMR(2, response);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 3));
    if (error)
        printf("luna: child network request failed event=%lu length=%lu\n",
               event, length_word);
    return error;
}
