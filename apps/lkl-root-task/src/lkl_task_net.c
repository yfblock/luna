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
#define TASK_NET_UDP_PORT 18081U
#define TASK_NET_TX_UDP_PORT 18082U
#define TASK_NET_PRESSURE_MIN_RX 24U

struct task_net_backend {
    struct lkl_netdev dev;
    unsigned char *tx_page;
    unsigned char *rx_page;
    size_t pending_rx;
    seL4_CPtr rx_ntfn;
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

static uint16_t task_ntohs(uint16_t value)
{
    return task_htons(value);
}

static uint32_t task_htonl(uint32_t value)
{
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

static uint32_t task_ntohl(uint32_t value)
{
    return task_htonl(value);
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
    int result;
    do {
        result = luna_lkl_task_manager_request(
            LUNA_ISOLATION_EVENT_NET_TX, (seL4_Word)total, 0);
        if (result == LUNA_NET_TX_RETRY) seL4_Yield();
    } while (result == LUNA_NET_TX_RETRY);
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
        seL4_Word badge = 0;
        seL4_Wait(backend->rx_ntfn, &badge);
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
    }
}

static void task_net_poll_hup(struct lkl_netdev *nd)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    __atomic_store_n(&backend->hup, 1, __ATOMIC_RELEASE);
    luna_lkl_task_manager_lock();
    (void)luna_lkl_task_manager_request(
        LUNA_ISOLATION_EVENT_NET_WAKE, 0, 0);
    luna_lkl_task_manager_unlock();
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
                                seL4_Word mac_word0, seL4_Word mac_word1,
                                seL4_CPtr rx_ntfn)
{
    if ((uintptr_t)io_base != LUNA_NET_IO_BASE ||
        io_size != LUNA_NET_IO_SIZE ||
        mac_word0 != LUNA_NET_MAC_WORD0 ||
        mac_word1 != LUNA_NET_MAC_WORD1 || !rx_ntfn)
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
    task_net.rx_ntfn = rx_ntfn;
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

static int task_net_control(seL4_Word control)
{
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request(
        LUNA_ISOLATION_EVENT_NET_CONTROL, control, 0);
    luna_lkl_task_manager_unlock();
    return result;
}

static int task_net_stats(seL4_Word *stats)
{
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request_value(
        LUNA_ISOLATION_EVENT_NET_STATS, 0, 0, stats);
    luna_lkl_task_manager_unlock();
    return result;
}

static int task_net_tx_stats(seL4_Word *stats)
{
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request_value(
        LUNA_ISOLATION_EVENT_NET_TX_STATS, 0, 0, stats);
    luna_lkl_task_manager_unlock();
    return result;
}

static int task_net_tx_stress_control(seL4_Word control)
{
    luna_lkl_task_manager_lock();
    int result = luna_lkl_task_manager_request(
        LUNA_ISOLATION_EVENT_NET_TX_STRESS, control, 0);
    luna_lkl_task_manager_unlock();
    return result;
}

int luna_lkl_task_net_pressure_smoke(void)
{
    static const unsigned char magic[8] = {
        'L', 'U', 'N', 'A', 'B', 'R', 'S', 'T'
    };
    unsigned char trigger[12];
    unsigned char packet[LUNA_NET_STRESS_PAYLOAD];
    struct lkl_sockaddr_in peer;
    long fd = -1;
    int masked = 0;
    int result = -1;
    uint64_t seen = 0;
    unsigned received = 0;
    unsigned long long start = luna_lkl_task_time();

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = LKL_AF_INET;
    peer.sin_port = task_htons(TASK_NET_UDP_PORT);
    peer.sin_addr.lkl_s_addr = task_ipv4(
        TASK_NET_IPV4_A, TASK_NET_IPV4_B, TASK_NET_IPV4_C,
        TASK_NET_PEER_D);
    memcpy(trigger, magic, sizeof(magic));
    uint16_t burst = task_htons((uint16_t)LUNA_NET_STRESS_BURST);
    uint16_t payload = task_htons((uint16_t)LUNA_NET_STRESS_PAYLOAD);
    memcpy(trigger + 8, &burst, sizeof(burst));
    memcpy(trigger + 10, &payload, sizeof(payload));

    fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM, LKL_IPPROTO_UDP);
    if (fd < 0 || task_net_control(1)) goto out;
    masked = 1;
    long syscall_result = lkl_sys_sendto((int)fd, trigger,
        sizeof(trigger), 0, (struct lkl_sockaddr *)&peer, sizeof(peer));
    if (syscall_result != (long)sizeof(trigger)) goto out;
    struct __lkl__kernel_timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 100000000,
    };
    if (lkl_sys_nanosleep(&delay, NULL) < 0) goto out;

    seL4_Word paused_stats = 0;
    if (task_net_stats(&paused_stats)) goto out;
    unsigned paused_high_water = luna_net_stats_unpack(
        paused_stats, LUNA_NET_STATS_HIGH_WATER_SHIFT);
    unsigned paused_backpressure = luna_net_stats_unpack(
        paused_stats, LUNA_NET_STATS_BACKPRESSURE_SHIFT);
    if (paused_high_water < TASK_NET_PRESSURE_MIN_RX ||
        !paused_backpressure)
        goto out;
    if (task_net_control(0)) goto out;
    masked = 0;

    for (unsigned attempt = 0;
         attempt < 30 && received < LUNA_NET_STRESS_BURST; attempt++) {
        struct lkl_pollfd pollfd = {
            .fd = (int)fd,
            .events = LKL_POLLIN,
        };
        syscall_result = lkl_sys_poll(&pollfd, 1, 200);
        if (syscall_result < 0) goto out;
        if (!syscall_result) continue;
        syscall_result = lkl_sys_recv((int)fd, packet, sizeof(packet),
                                      LKL_MSG_DONTWAIT);
        if (syscall_result != (long)LUNA_NET_STRESS_PAYLOAD) goto out;
        uint16_t sequence_word = 0, count_word = 0;
        memcpy(&sequence_word, packet, sizeof(sequence_word));
        memcpy(&count_word, packet + 2, sizeof(count_word));
        unsigned sequence = task_ntohs(sequence_word);
        unsigned count = task_ntohs(count_word);
        if (count != LUNA_NET_STRESS_BURST ||
            sequence >= LUNA_NET_STRESS_BURST)
            goto out;
        for (size_t i = 4; i < sizeof(packet); i++) {
            if (packet[i] != (unsigned char)sequence) goto out;
        }
        uint64_t bit = 1ULL << sequence;
        if (!(seen & bit)) {
            seen |= bit;
            received++;
        }
    }

    seL4_Word stats = 0;
    if (task_net_stats(&stats)) goto out;
    unsigned high_water = luna_net_stats_unpack(
        stats, LUNA_NET_STATS_HIGH_WATER_SHIFT);
    unsigned backpressure = luna_net_stats_unpack(
        stats, LUNA_NET_STATS_BACKPRESSURE_SHIFT);
    unsigned drops = luna_net_stats_unpack(
        stats, LUNA_NET_STATS_DROPS_SHIFT);
    unsigned empty_fetches = luna_net_stats_unpack(
        stats, LUNA_NET_STATS_EMPTY_FETCHES_SHIFT);
    if (received < TASK_NET_PRESSURE_MIN_RX ||
        high_water < TASK_NET_PRESSURE_MIN_RX || !backpressure ||
        empty_fetches)
        goto out;
    unsigned long long elapsed = luna_lkl_task_time() - start;
    if (!elapsed) goto out;
    lkl_printf("LUNA_NET_QUEUE_STATS received=%u high_water=%u "
               "backpressure=%u drops=%u empty_fetches=%u "
               "elapsed_ns=%llu\n",
               received, high_water, backpressure, drops, empty_fetches,
               elapsed);
    result = 0;

out:
    if (masked) (void)task_net_control(0);
    if (fd >= 0) lkl_sys_close((unsigned int)fd);
    if (result)
        lkl_printf("luna-lkl-task: network pressure smoke failed\n");
    return result;
}

int luna_lkl_task_net_tx_pressure_smoke(void)
{
    static const unsigned char magic[8] = {
        'L', 'U', 'N', 'A', 'T', 'X', '2', '5'
    };
    static const unsigned char ack_magic[8] = {
        'L', 'U', 'N', 'A', 'T', 'X', 'O', 'K'
    };
    unsigned char packet[LUNA_NET_TX_STRESS_PAYLOAD];
    unsigned char ack[16];
    struct lkl_sockaddr_in peer;
    long fd = -1;
    int result = -1;
    const char *failure = "socket";
    long failure_detail = 0;
    unsigned failure_attempt = 0;
    uint32_t failure_sequence = 0;
    unsigned long long start = luna_lkl_task_time();
    int stress_gate_armed = 0;

    memset(&peer, 0, sizeof(peer));
    peer.sin_family = LKL_AF_INET;
    peer.sin_port = task_htons(TASK_NET_TX_UDP_PORT);
    peer.sin_addr.lkl_s_addr = task_ipv4(
        TASK_NET_IPV4_A, TASK_NET_IPV4_B, TASK_NET_IPV4_C,
        TASK_NET_PEER_D);
    fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM, LKL_IPPROTO_UDP);
    if (fd < 0) goto out;
    if (task_net_tx_stress_control(1)) {
        failure = "gate-arm";
        goto out;
    }
    stress_gate_armed = 1;

    int acknowledged = 0;
    for (unsigned attempt = 0; attempt < 3 && !acknowledged; attempt++) {
        failure_attempt = attempt + 1U;
        for (uint32_t sequence = 0;
             sequence < LUNA_NET_TX_STRESS_PACKETS; sequence++) {
            memcpy(packet, magic, sizeof(magic));
            uint32_t sequence_word = task_htonl(sequence);
            uint32_t count_word =
                task_htonl((uint32_t)LUNA_NET_TX_STRESS_PACKETS);
            memcpy(packet + 8, &sequence_word, sizeof(sequence_word));
            memcpy(packet + 12, &count_word, sizeof(count_word));
            memset(packet + 16, (unsigned char)sequence,
                   sizeof(packet) - 16);
            long syscall_result = lkl_sys_sendto(
                (int)fd, packet, sizeof(packet), 0,
                (struct lkl_sockaddr *)&peer, sizeof(peer));
            if (syscall_result != (long)sizeof(packet)) {
                failure = "send";
                failure_detail = syscall_result;
                failure_sequence = sequence;
                goto out;
            }
        }
        stress_gate_armed = 0;

        struct lkl_pollfd pollfd = {
            .fd = (int)fd,
            .events = LKL_POLLIN,
        };
        long syscall_result = lkl_sys_poll(&pollfd, 1, 3000);
        if (syscall_result < 0) {
            failure = "ack-poll";
            failure_detail = syscall_result;
            goto out;
        }
        if (!syscall_result) continue;
        syscall_result = lkl_sys_recv((int)fd, ack, sizeof(ack),
                                      LKL_MSG_DONTWAIT);
        if (syscall_result != (long)sizeof(ack) ||
            memcmp(ack, ack_magic, sizeof(ack_magic))) {
            failure = "ack-payload";
            failure_detail = syscall_result;
            goto out;
        }
        acknowledged = 1;
    }
    if (!acknowledged) {
        failure = "ack-timeout";
        goto out;
    }
    uint32_t count_word = 0, bytes_word = 0;
    memcpy(&count_word, ack + 8, sizeof(count_word));
    memcpy(&bytes_word, ack + 12, sizeof(bytes_word));
    unsigned count = task_ntohl(count_word);
    unsigned bytes = task_ntohl(bytes_word);
    if (count != LUNA_NET_TX_STRESS_PACKETS ||
        bytes != LUNA_NET_TX_STRESS_PACKETS * LUNA_NET_TX_STRESS_PAYLOAD) {
        failure = "ack-count";
        failure_detail = count;
        goto out;
    }

    seL4_Word stats = 0;
    if (task_net_tx_stats(&stats)) {
        failure = "stats-request";
        goto out;
    }
    unsigned high_water = luna_net_stats_unpack(
        stats, LUNA_NET_TX_STATS_HIGH_WATER_SHIFT);
    unsigned backpressure = luna_net_stats_unpack(
        stats, LUNA_NET_TX_STATS_BACKPRESSURE_SHIFT);
    unsigned retries = luna_net_stats_unpack(
        stats, LUNA_NET_TX_STATS_DRIVER_RETRIES_SHIFT);
    unsigned completed = luna_net_stats_unpack(
        stats, LUNA_NET_TX_STATS_COMPLETED_SHIFT);
    unsigned long long elapsed = luna_lkl_task_time() - start;
    if (high_water < 2 || !backpressure ||
        completed < LUNA_NET_TX_STRESS_PACKETS || !elapsed) {
        lkl_printf("luna-lkl-task: TX stats high_water=%u "
                   "backpressure=%u retries=%u completed=%u "
                   "elapsed=%llu\n", high_water, backpressure, retries,
                   completed, elapsed);
        failure = "stats-values";
        failure_detail = completed;
        goto out;
    }
    lkl_printf("LUNA_NET_TX_QUEUE_STATS sent=%lu high_water=%u "
               "backpressure=%u driver_retries=%u completed=%u "
               "elapsed_ns=%llu\n",
               (unsigned long)LUNA_NET_TX_STRESS_PACKETS, high_water,
               backpressure, retries, completed, elapsed);
    result = 0;

out:
    if (stress_gate_armed) (void)task_net_tx_stress_control(0);
    if (fd >= 0) lkl_sys_close((unsigned int)fd);
    if (result)
        lkl_printf("luna-lkl-task: network TX pressure smoke failed "
                   "stage=%s attempt=%u sequence=%lu detail=%ld\n",
                   failure, failure_attempt,
                   (unsigned long)failure_sequence, failure_detail);
    return result;
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
