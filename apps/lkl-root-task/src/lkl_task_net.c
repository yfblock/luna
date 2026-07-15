/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Child-side LKL virtio-net backend. Ethernet packets cross a bounded batch
 * window and a validated manager Endpoint protocol; PCI/DMA stay out of the
 * child CSpace.
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
#define TASK_NET_TX_COALESCE_NS 5000000ULL
#define TASK_NET_THROUGHPUT_SAMPLES 7U
#define TASK_NET_THROUGHPUT_PACKETS 24U

struct task_net_packet {
    size_t length;
    unsigned char data[LUNA_NET_PACKET_SIZE];
};

struct task_net_backend {
    struct lkl_netdev dev;
    struct luna_net_batch_header *header;
    unsigned char *tx_slots;
    unsigned char *rx_slots;
    struct task_net_packet tx_queue[LUNA_NET_CHILD_TX_QUEUE_COUNT];
    volatile unsigned tx_head;
    volatile unsigned tx_tail;
    volatile int tx_lock;
    volatile int tx_stop;
    volatile int tx_error;
    struct lkl_sem *tx_sem;
    lkl_thread_t tx_thread;
    unsigned rx_count;
    unsigned rx_index;
    size_t pending_rx;
    seL4_CPtr rx_ntfn;
    volatile int hup;
    int configured;
    volatile unsigned long long tx_ipc;
    volatile unsigned long long tx_batches;
    volatile unsigned long long tx_packets;
    volatile unsigned long long tx_copies;
    volatile unsigned long long tx_backpressure;
    volatile unsigned long long rx_ipc;
    volatile unsigned long long rx_batches;
    volatile unsigned long long rx_packets;
    volatile unsigned long long rx_copies;
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

static void task_net_tx_lock(struct task_net_backend *backend)
{
    while (__atomic_test_and_set(&backend->tx_lock, __ATOMIC_ACQUIRE))
        seL4_Yield();
}

static void task_net_tx_unlock(struct task_net_backend *backend)
{
    __atomic_clear(&backend->tx_lock, __ATOMIC_RELEASE);
}

static void task_net_tx_worker(void *argument)
{
    struct task_net_backend *backend = argument;
    for (;;) {
        lkl_host_ops.sem_down(backend->tx_sem);
        if (__atomic_load_n(&backend->tx_stop, __ATOMIC_ACQUIRE)) break;
        unsigned long long coalesce_start = luna_lkl_task_time();
        for (;;) {
            task_net_tx_lock(backend);
            unsigned queued = (backend->tx_head +
                               LUNA_NET_CHILD_TX_QUEUE_COUNT -
                               backend->tx_tail) %
                              LUNA_NET_CHILD_TX_QUEUE_COUNT;
            task_net_tx_unlock(backend);
            if (queued >= LUNA_NET_BATCH_SLOTS ||
                luna_lkl_task_time() - coalesce_start >=
                    TASK_NET_TX_COALESCE_NS)
                break;
            seL4_Yield();
        }
        for (;;) {
            task_net_tx_lock(backend);
            unsigned tail = backend->tx_tail;
            unsigned head = backend->tx_head;
            if (tail == head) {
                task_net_tx_unlock(backend);
                break;
            }
            unsigned count = 0;
            while (tail != head && count < LUNA_NET_BATCH_SLOTS) {
                struct task_net_packet *packet = &backend->tx_queue[tail];
                backend->header->tx_lengths[count] = packet->length;
                memcpy(backend->tx_slots +
                           count * LUNA_NET_PACKET_SIZE,
                       packet->data, packet->length);
                tail = (tail + 1U) % LUNA_NET_CHILD_TX_QUEUE_COUNT;
                count++;
            }
            backend->header->tx_count = count;
            __sync_synchronize();
            task_net_tx_unlock(backend);

            seL4_Word accepted = 0;
            luna_lkl_task_manager_lock();
            int result = luna_lkl_task_manager_request_value(
                LUNA_ISOLATION_EVENT_NET_TX_BATCH, count, 0, &accepted);
            luna_lkl_task_manager_unlock();
            __atomic_fetch_add(&backend->tx_ipc, 1ULL, __ATOMIC_RELAXED);
            if (result || accepted > count) {
                __atomic_store_n(&backend->tx_error, 1, __ATOMIC_RELEASE);
                break;
            }
            if (!accepted) {
                __atomic_fetch_add(&backend->tx_backpressure, 1ULL,
                                   __ATOMIC_RELAXED);
                seL4_Yield();
                continue;
            }
            task_net_tx_lock(backend);
            for (seL4_Word i = 0; i < accepted; i++) {
                backend->tx_queue[backend->tx_tail].length = 0;
                backend->tx_tail = (backend->tx_tail + 1U) %
                                   LUNA_NET_CHILD_TX_QUEUE_COUNT;
            }
            task_net_tx_unlock(backend);
            __atomic_fetch_add(&backend->tx_batches, 1ULL,
                               __ATOMIC_RELAXED);
            __atomic_fetch_add(&backend->tx_packets,
                               (unsigned long long)accepted,
                               __ATOMIC_RELAXED);
            __atomic_fetch_add(&backend->tx_copies,
                               (unsigned long long)accepted,
                               __ATOMIC_RELAXED);
        }
    }
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
        total += iov[i].iov_len;
    }
    if (!total) return -1;
    for (unsigned attempt = 0; attempt < 100000U; attempt++) {
        task_net_tx_lock(backend);
        unsigned head = backend->tx_head;
        unsigned next = (head + 1U) % LUNA_NET_CHILD_TX_QUEUE_COUNT;
        if (next != backend->tx_tail) {
            int was_empty = head == backend->tx_tail;
            struct task_net_packet *packet = &backend->tx_queue[head];
            size_t offset = 0;
            for (int i = 0; i < count; i++) {
                memcpy(packet->data + offset, iov[i].iov_base,
                       iov[i].iov_len);
                offset += iov[i].iov_len;
            }
            packet->length = total;
            backend->tx_head = next;
            task_net_tx_unlock(backend);
            if (was_empty) lkl_host_ops.sem_up(backend->tx_sem);
            return (int)total;
        }
        task_net_tx_unlock(backend);
        __atomic_fetch_add(&backend->tx_backpressure, 1ULL,
                           __ATOMIC_RELAXED);
        if (__atomic_load_n(&backend->tx_error, __ATOMIC_ACQUIRE)) return -1;
        lkl_host_ops.sem_up(backend->tx_sem);
        seL4_Yield();
    }
    return -1;
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
        memcpy(iov[i].iov_base,
               backend->rx_slots +
                   backend->rx_index * LUNA_NET_PACKET_SIZE + offset,
               chunk);
        offset += chunk;
        remaining -= chunk;
    }
    backend->pending_rx = 0;
    backend->rx_index++;
    __atomic_fetch_add(&backend->rx_packets, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&backend->rx_copies, 1ULL, __ATOMIC_RELAXED);
    return (int)offset;
}

static int task_net_poll(struct lkl_netdev *nd)
{
    struct task_net_backend *backend =
        (struct task_net_backend *)nd;
    for (;;) {
        if (__atomic_load_n(&backend->hup, __ATOMIC_ACQUIRE))
            return LKL_DEV_NET_POLL_HUP;
        if (backend->rx_index < backend->rx_count) {
            size_t length = (size_t)backend->header->rx_lengths[
                backend->rx_index];
            if (!length || length > LUNA_NET_PACKET_SIZE) return -1;
            backend->pending_rx = length;
            return LKL_DEV_NET_POLL_RX;
        }
        seL4_Word badge = 0;
        seL4_Wait(backend->rx_ntfn, &badge);
        if (__atomic_load_n(&backend->hup, __ATOMIC_ACQUIRE))
            return LKL_DEV_NET_POLL_HUP;
        seL4_Word packets = 0;
        luna_lkl_task_manager_lock();
        int result = luna_lkl_task_manager_request_value(
            LUNA_ISOLATION_EVENT_NET_RX_BATCH, LUNA_NET_BATCH_SLOTS, 0,
            &packets);
        luna_lkl_task_manager_unlock();
        __atomic_fetch_add(&backend->rx_ipc, 1ULL, __ATOMIC_RELAXED);
        if (result || packets > LUNA_NET_BATCH_SLOTS) return -1;
        if (packets) {
            backend->rx_count = (unsigned)packets;
            backend->rx_index = 0;
            __atomic_fetch_add(&backend->rx_batches, 1ULL,
                               __ATOMIC_RELAXED);
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
    backend->rx_count = 0;
    backend->rx_index = 0;
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
    task_net.header = io_base;
    task_net.tx_slots = (unsigned char *)io_base + LUNA_NET_TX_OFFSET;
    task_net.rx_slots = (unsigned char *)io_base + LUNA_NET_RX_OFFSET;
    task_net.rx_ntfn = rx_ntfn;
    task_net.configured = 1;
    task_net_id = -1;
    task_net_ifindex = -1;
    return 0;
}

int luna_lkl_task_net_add(void)
{
    if (!task_net.configured || task_net_id >= 0) return -1;
    task_net.tx_sem = lkl_host_ops.sem_alloc(0);
    if (!task_net.tx_sem) return -1;
    task_net.tx_thread = lkl_host_ops.thread_create(task_net_tx_worker,
                                                    &task_net);
    if (!task_net.tx_thread) {
        lkl_host_ops.sem_free(task_net.tx_sem);
        task_net.tx_sem = NULL;
        return -1;
    }
    struct lkl_netdev_args args = {
        .mac = task_net.dev.mac,
        .offload = 0,
    };
    task_net.hup = 0;
    task_net_id = lkl_netdev_add(&task_net.dev, &args);
    if (task_net_id < 0) {
        lkl_printf("luna-lkl-task: virtio net add failed: %d\n",
                   task_net_id);
        __atomic_store_n(&task_net.tx_stop, 1, __ATOMIC_RELEASE);
        lkl_host_ops.sem_up(task_net.tx_sem);
        lkl_host_ops.thread_join(task_net.tx_thread);
        lkl_host_ops.sem_free(task_net.tx_sem);
        task_net.tx_sem = NULL;
        task_net.tx_thread = 0;
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

static int task_net_rx_throughput_benchmark(void)
{
    static const unsigned char magic[8] = {
        'L', 'U', 'N', 'A', 'B', 'R', 'S', 'T'
    };
    unsigned long long samples[TASK_NET_THROUGHPUT_SAMPLES];
    unsigned char trigger[12];
    unsigned char packet[LUNA_NET_STRESS_PAYLOAD];
    struct lkl_sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = LKL_AF_INET;
    peer.sin_port = task_htons(TASK_NET_UDP_PORT);
    peer.sin_addr.lkl_s_addr = task_ipv4(
        TASK_NET_IPV4_A, TASK_NET_IPV4_B, TASK_NET_IPV4_C,
        TASK_NET_PEER_D);
    memcpy(trigger, magic, sizeof(magic));
    uint16_t burst = task_htons((uint16_t)TASK_NET_THROUGHPUT_PACKETS);
    uint16_t payload = task_htons((uint16_t)LUNA_NET_STRESS_PAYLOAD);
    memcpy(trigger + 8, &burst, sizeof(burst));
    memcpy(trigger + 10, &payload, sizeof(payload));

    long fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM,
                             LKL_IPPROTO_UDP);
    if (fd < 0) return -1;
    for (unsigned sample = 0; sample < TASK_NET_THROUGHPUT_SAMPLES;
         sample++) {
        uint64_t seen = 0;
        unsigned received = 0;
        long result = lkl_sys_sendto(
            (int)fd, trigger, sizeof(trigger), 0,
            (struct lkl_sockaddr *)&peer, sizeof(peer));
        if (result != (long)sizeof(trigger)) goto fail;
        unsigned long long start = luna_lkl_task_time();
        for (unsigned attempt = 0;
             attempt < 100 && received < TASK_NET_THROUGHPUT_PACKETS;
             attempt++) {
            struct lkl_pollfd pollfd = {
                .fd = (int)fd,
                .events = LKL_POLLIN,
            };
            result = lkl_sys_poll(&pollfd, 1, 100);
            if (result < 0) goto fail;
            if (!result) continue;
            result = lkl_sys_recv((int)fd, packet, sizeof(packet),
                                  LKL_MSG_DONTWAIT);
            if (result != (long)LUNA_NET_STRESS_PAYLOAD) goto fail;
            uint16_t sequence_word = 0, count_word = 0;
            memcpy(&sequence_word, packet, sizeof(sequence_word));
            memcpy(&count_word, packet + 2, sizeof(count_word));
            unsigned sequence = task_ntohs(sequence_word);
            unsigned count = task_ntohs(count_word);
            if (count != TASK_NET_THROUGHPUT_PACKETS ||
                sequence >= TASK_NET_THROUGHPUT_PACKETS)
                goto fail;
            uint64_t bit = 1ULL << sequence;
            if (!(seen & bit)) {
                seen |= bit;
                received++;
            }
        }
        if (received != TASK_NET_THROUGHPUT_PACKETS) goto fail;
        samples[sample] = luna_lkl_task_time() - start;
        if (!samples[sample]) goto fail;
        lkl_printf("LUNA_NET_RX_THROUGHPUT_SAMPLE ns=%llu\n",
                   samples[sample]);
    }
    if (lkl_sys_close((unsigned int)fd)) return -1;
    for (unsigned i = 1; i < TASK_NET_THROUGHPUT_SAMPLES; i++) {
        unsigned long long value = samples[i];
        unsigned j = i;
        while (j && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    unsigned long long p50 = samples[(TASK_NET_THROUGHPUT_SAMPLES - 1) / 2];
    unsigned long long p95 = samples[TASK_NET_THROUGHPUT_SAMPLES - 1];
    lkl_printf("LUNA_NET_RX_THROUGHPUT_OK packets=%lu bytes=%lu samples=%u "
               "p50_ns=%llu p95_ns=%llu p99_ns=%llu\n",
               (unsigned long)TASK_NET_THROUGHPUT_PACKETS,
               (unsigned long)(TASK_NET_THROUGHPUT_PACKETS *
                               LUNA_NET_STRESS_PAYLOAD),
               TASK_NET_THROUGHPUT_SAMPLES, p50, p95, p95);
    return 0;

fail:
    lkl_sys_close((unsigned int)fd);
    return -1;
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
    if (task_net_rx_throughput_benchmark()) goto out;
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
    lkl_host_ops.sem_up(task_net.tx_sem);
    for (unsigned attempt = 0; attempt < 100000U; attempt++) {
        if (__atomic_load_n(&task_net.tx_head, __ATOMIC_ACQUIRE) ==
            __atomic_load_n(&task_net.tx_tail, __ATOMIC_ACQUIRE))
            break;
        seL4_Yield();
    }
    if (__atomic_load_n(&task_net.tx_head, __ATOMIC_ACQUIRE) !=
        __atomic_load_n(&task_net.tx_tail, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&task_net.tx_error, __ATOMIC_ACQUIRE))
        return -1;
    __atomic_store_n(&task_net.tx_stop, 1, __ATOMIC_RELEASE);
    lkl_host_ops.sem_up(task_net.tx_sem);
    if (lkl_host_ops.thread_join(task_net.tx_thread)) return -1;
    lkl_host_ops.sem_free(task_net.tx_sem);
    task_net.tx_sem = NULL;
    task_net.tx_thread = 0;
    lkl_printf("LUNA_NET_BATCH_COUNTERS tx_ipc=%llu tx_batches=%llu "
               "tx_packets=%llu tx_copies=%llu tx_backpressure=%llu "
               "rx_ipc=%llu rx_batches=%llu rx_packets=%llu "
               "rx_copies=%llu\n",
               task_net.tx_ipc, task_net.tx_batches, task_net.tx_packets,
               task_net.tx_copies, task_net.tx_backpressure,
               task_net.rx_ipc, task_net.rx_batches, task_net.rx_packets,
               task_net.rx_copies);
    lkl_netdev_remove(task_net_id);
    lkl_netdev_free(&task_net.dev);
    task_net_id = -1;
    task_net_ifindex = -1;
    return 0;
}
