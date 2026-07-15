/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Child-side LKL virtio-net backend. Ethernet packets cross only two bounded
 * shared pages and a validated manager Endpoint protocol; PCI/DMA stay out of
 * the child CSpace.
 */
#include "luna_lkl_task_host.h"

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm/syscalls.h>
#include <lkl/linux/icmp.h>
#include <lkl/linux/if_ether.h>
#include <lkl/linux/in.h>
#include <lkl/linux/ip.h>
#include <lkl/linux/poll.h>
#include <lkl/linux/socket.h>

#include <stdint.h>
#include <string.h>

#define TASK_NET_IPV4_A 10U
#define TASK_NET_IPV4_B 0U
#define TASK_NET_IPV4_C 2U
#define TASK_NET_IPV4_D 15U
#define TASK_NET_PEER_D 2U
#define TASK_NET_TCP_PORT 18080U

struct task_net_backend {
    struct lkl_netdev dev;
    unsigned char *tx_page;
    unsigned char *rx_page;
    size_t pending_rx;
    volatile int hup;
    int configured;
};

static struct task_net_backend task_net;
static int task_net_id = -1;
static int task_net_ifindex = -1;

static unsigned int task_ipv4(unsigned int a, unsigned int b,
                              unsigned int c, unsigned int d)
{
    union {
        unsigned char bytes[4];
        unsigned int value;
    } address = { .bytes = { a, b, c, d } };
    return address.value;
}

static uint16_t task_htons(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint16_t task_checksum(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint32_t sum = 0;
    while (length > 1) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)bytes[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return task_htons((uint16_t)~sum);
}

static int task_net_tx(struct lkl_netdev *nd, struct iovec *iov, int count)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    if (!backend->configured || count < 0) return -1;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (!iov[i].iov_base ||
            iov[i].iov_len > LUNA_NET_PACKET_SIZE - total)
            return -1;
        memcpy(backend->tx_page + total, iov[i].iov_base, iov[i].iov_len);
        total += iov[i].iov_len;
    }
    if (!total) return -1;
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request(
        LUNA_ISOLATION_EVENT_NET_TX, (seL4_Word)total, 0);
    luna_lkl_task_manager_unlock();
    return result ? -1 : (int)total;
}

static int task_net_rx(struct lkl_netdev *nd, struct iovec *iov, int count)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    if (!backend->pending_rx || count < 0) return -1;
    size_t remaining = backend->pending_rx;
    size_t offset = 0;
    for (int i = 0; i < count && remaining; i++) {
        size_t chunk = iov[i].iov_len < remaining ? iov[i].iov_len :
                       remaining;
        if (!iov[i].iov_base) return -1;
        memcpy(iov[i].iov_base, backend->rx_page + offset, chunk);
        offset += chunk;
        remaining -= chunk;
    }
    backend->pending_rx = 0;
    return (int)offset;
}

static int task_net_poll(struct lkl_netdev *nd)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    for (;;) {
        if (__atomic_load_n(&backend->hup, __ATOMIC_ACQUIRE))
            return LKL_DEV_NET_POLL_HUP;
        seL4_Word length = 0;
        luna_lkl_task_manager_lock();
        int result = luna_lkl_task_manager_request_value(
            LUNA_ISOLATION_EVENT_NET_RX, 0, 0, &length);
        luna_lkl_task_manager_unlock();
        if (result || length > LUNA_NET_PACKET_SIZE) return -1;
        if (length) {
            backend->pending_rx = (size_t)length;
            return LKL_DEV_NET_POLL_RX;
        }
        seL4_Yield();
    }
}

static void task_net_poll_hup(struct lkl_netdev *nd)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    __atomic_store_n(&backend->hup, 1, __ATOMIC_RELEASE);
}

static void task_net_free(struct lkl_netdev *nd)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    backend->configured = 0;
    backend->pending_rx = 0;
}

static struct lkl_dev_net_ops task_net_ops = {
    .tx = task_net_tx,
    .rx = task_net_rx,
    .poll = task_net_poll,
    .poll_hup = task_net_poll_hup,
    .free = task_net_free,
};

int luna_lkl_task_configure_net(void *io_base, unsigned long io_size,
                                seL4_Word mac_word0, seL4_Word mac_word1)
{
    if ((uintptr_t)io_base != LUNA_NET_IO_BASE ||
        io_size != LUNA_NET_IO_SIZE ||
        mac_word0 != LUNA_NET_MAC_WORD0 ||
        mac_word1 != LUNA_NET_MAC_WORD1)
        return -1;
    memset(&task_net, 0, sizeof(task_net));
    task_net.dev.ops = &task_net_ops;
    task_net.dev.has_vnet_hdr = 0;
    task_net.dev.mac[0] = 0x52;
    task_net.dev.mac[1] = 0x54;
    task_net.dev.mac[2] = 0x00;
    task_net.dev.mac[3] = 0x12;
    task_net.dev.mac[4] = 0x34;
    task_net.dev.mac[5] = 0x56;
    task_net.tx_page = (unsigned char *)io_base + LUNA_NET_TX_OFFSET;
    task_net.rx_page = (unsigned char *)io_base + LUNA_NET_RX_OFFSET;
    task_net.configured = 1;
    task_net_id = -1;
    task_net_ifindex = -1;
    return 0;
}

int luna_lkl_task_net_add(void)
{
    if (!task_net.configured || task_net_id >= 0) return -1;
    struct lkl_netdev_args args = {
        .mac = task_net.dev.mac,
        .offload = 0,
    };
    task_net.hup = 0;
    task_net_id = lkl_netdev_add(&task_net.dev, &args);
    if (task_net_id < 0) {
        lkl_printf("luna-lkl-task: virtio net add failed: %d\n",
                   task_net_id);
        return -1;
    }
    return 0;
}

int luna_lkl_task_net_prepare(void)
{
    if (task_net_id < 0) return -1;
    unsigned int address = task_ipv4(TASK_NET_IPV4_A, TASK_NET_IPV4_B,
                                     TASK_NET_IPV4_C, TASK_NET_IPV4_D);
    unsigned int gateway = task_ipv4(TASK_NET_IPV4_A, TASK_NET_IPV4_B,
                                     TASK_NET_IPV4_C, TASK_NET_PEER_D);
    task_net_ifindex = lkl_netdev_get_ifindex(task_net_id);
    if (task_net_ifindex < 0 ||
        lkl_if_set_ipv4(task_net_ifindex, address, 24) < 0 ||
        lkl_if_up(task_net_ifindex) < 0 ||
        lkl_if_set_ipv4_gateway(task_net_ifindex, address, 24,
                                gateway) < 0) {
        lkl_printf("luna-lkl-task: IPv4 configuration failed ifindex=%d\n",
                   task_net_ifindex);
        return -1;
    }
    return 0;
}

static int task_icmp_smoke(void)
{
    struct lkl_sockaddr_in peer;
    struct lkl_icmphdr request;
    unsigned char response[128];
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = LKL_AF_INET;
    peer.sin_addr.lkl_s_addr = task_ipv4(
        TASK_NET_IPV4_A, TASK_NET_IPV4_B, TASK_NET_IPV4_C,
        TASK_NET_PEER_D);
    memset(&request, 0, sizeof(request));
    request.type = LKL_ICMP_ECHO;
    request.un.echo.id = task_htons(0x4c55U);
    request.un.echo.sequence = task_htons(0x0025U);
    request.checksum = task_checksum(&request, sizeof(request));

    long fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_RAW,
                             LKL_IPPROTO_ICMP);
    if (fd < 0) return (int)fd;
    long result = lkl_sys_sendto((int)fd, &request, sizeof(request), 0,
        (struct lkl_sockaddr *)&peer, sizeof(peer));
    if (result != (long)sizeof(request)) goto fail;
    for (int attempt = 0; attempt < 4; attempt++) {
        struct lkl_pollfd pollfd = {
            .fd = (int)fd,
            .events = LKL_POLLIN,
        };
        result = lkl_sys_poll(&pollfd, 1, 1000);
        if (result <= 0) goto fail;
        result = lkl_sys_recv((int)fd, response, sizeof(response),
                              LKL_MSG_DONTWAIT);
        if (result < (long)(sizeof(struct lkl_iphdr) +
                            sizeof(struct lkl_icmphdr)))
            continue;
        struct lkl_iphdr *ip = (struct lkl_iphdr *)response;
        size_t ip_length = (size_t)ip->ihl * 4U;
        if (ip_length + sizeof(struct lkl_icmphdr) > (size_t)result)
            continue;
        struct lkl_icmphdr *icmp =
            (struct lkl_icmphdr *)(response + ip_length);
        if (icmp->type == LKL_ICMP_ECHOREPLY && !icmp->code &&
            icmp->un.echo.id == request.un.echo.id &&
            icmp->un.echo.sequence == request.un.echo.sequence) {
            lkl_sys_close((unsigned int)fd);
            return 0;
        }
    }
fail:
    lkl_sys_close((unsigned int)fd);
    return -1;
}

static int task_tcp_smoke(void)
{
    static const char payload[] = "luna-phase-2.5-tcp";
    char response[sizeof(payload)] = {0};
    struct lkl_sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = LKL_AF_INET;
    peer.sin_port = task_htons(TASK_NET_TCP_PORT);
    peer.sin_addr.lkl_s_addr = task_ipv4(
        TASK_NET_IPV4_A, TASK_NET_IPV4_B, TASK_NET_IPV4_C,
        TASK_NET_PEER_D);
    long fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM,
                             LKL_IPPROTO_TCP);
    if (fd < 0) return (int)fd;
    long result = lkl_sys_connect((int)fd,
        (struct lkl_sockaddr *)&peer, sizeof(peer));
    if (result < 0) goto fail;
    result = lkl_sys_write((unsigned int)fd, (void *)payload,
                           sizeof(payload) - 1);
    if (result != (long)sizeof(payload) - 1) goto fail;
    struct lkl_pollfd pollfd = {
        .fd = (int)fd,
        .events = LKL_POLLIN,
    };
    result = lkl_sys_poll(&pollfd, 1, 3000);
    if (result <= 0) goto fail;
    result = lkl_sys_read((unsigned int)fd, response,
                          sizeof(payload) - 1);
    if (result != (long)sizeof(payload) - 1 ||
        memcmp(response, payload, sizeof(payload) - 1))
        goto fail;
    lkl_sys_close((unsigned int)fd);
    return 0;
fail:
    lkl_sys_close((unsigned int)fd);
    return -1;
}

int luna_lkl_task_net_smoke(void)
{
    int result = task_icmp_smoke();
    if (result) {
        lkl_printf("luna-lkl-task: ICMP smoke failed: %d\n", result);
        return -1;
    }
    result = task_tcp_smoke();
    if (result) {
        lkl_printf("luna-lkl-task: TCP smoke failed: %d\n", result);
        return -1;
    }
    return 0;
}

int luna_lkl_task_net_finish(void)
{
    if (task_net_id < 0) return 0;
    lkl_netdev_remove(task_net_id);
    lkl_netdev_free(&task_net.dev);
    task_net_id = -1;
    task_net_ifindex = -1;
    return 0;
}
