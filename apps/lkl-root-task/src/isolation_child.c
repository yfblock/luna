/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal payload used to establish the process boundary that LKL will move
 * into. It receives a send-only event endpoint, a receive-only command
 * endpoint, plus the capabilities installed by sel4utils for its own
 * CSpace, VSpace and TCB.
 */
#include "luna_isolation_protocol.h"
#include "luna_lkl_task_host.h"
#include "luna_shell.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <lkl_host.h>
#include <string.h>

struct child_boot_state {
    seL4_CPtr control_ep;
    seL4_CPtr command_ep;
    seL4_Word mode;
    seL4_Word private_addr;
};

/* The initial host thread becomes LKL's init task and crosses setjmp/longjmp
 * scheduler boundaries. Keep lifecycle arguments out of its stack frame so
 * they remain authoritative after lkl_sys_halt(). */
static struct child_boot_state child_boot;

static seL4_Word parse_word(const char *s)
{
    seL4_Word value = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (seL4_Word)(*s - '0');
        s++;
    }
    return value;
}

static void send_event(seL4_CPtr control_ep, seL4_Word event, seL4_Word detail)
{
    seL4_SetMR(0, event);
    seL4_SetMR(1, detail);
    seL4_Send(control_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static seL4_MessageInfo_t receive_command(seL4_CPtr command_ep,
                                          enum luna_manager_command expected)
{
    seL4_Word badge = 0;
    seL4_MessageInfo_t tag = seL4_Recv(command_ep, &badge);
    if (badge || seL4_MessageInfo_get_label(tag) != 0 ||
        seL4_MessageInfo_get_length(tag) < 1 || seL4_GetMR(0) != expected) {
        for (;;) seL4_Yield();
    }
    return tag;
}

static __attribute__((noreturn)) void finish_child(void)
{
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_LKL_HALT_OK,
               child_boot.mode);
    send_event(child_boot.control_ep,
               LUNA_ISOLATION_EVENT_ALLOCATOR_RELEASED, child_boot.mode);

    if (child_boot.mode == LUNA_ISOLATION_MODE_FAULT) {
        /* The manager maps this address only in its own VSpace. Reaching the
         * next send would prove that the child can see manager-private data. */
        volatile seL4_Word secret =
            *(volatile seL4_Word *)(uintptr_t)child_boot.private_addr;
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_PRIVATE_PAGE_VISIBLE, secret);
    } else {
        send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_DONE,
                   child_boot.mode);
    }

    seL4_TCB_Suspend(SEL4UTILS_TCB_SLOT);
    for (;;) seL4_Yield();
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        for (;;) seL4_Yield();
    }

    child_boot.control_ep = (seL4_CPtr)parse_word(argv[0]);
    child_boot.command_ep = (seL4_CPtr)parse_word(argv[1]);
    child_boot.mode = parse_word(argv[2]);
    child_boot.private_addr = parse_word(argv[3]);

    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_READY,
               child_boot.mode);

    /* Volatile keeps a real relocation to lkl_init in this ELF and proves that
     * the LKL kernel object has moved into the isolated task image. */
    volatile uintptr_t lkl_link_anchor = (uintptr_t)&lkl_init;
    if (!lkl_link_anchor) for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_LKL_LINKED,
               child_boot.mode);

    struct luna_task_thread_resource resources[LUNA_RESOURCE_SLOTS];
    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        seL4_MessageInfo_t config_tag =
            receive_command(child_boot.command_ep,
                            LUNA_COMMAND_CONFIGURE_SLOT);
        if (seL4_MessageInfo_get_length(config_tag) != 8 ||
            seL4_GetMR(1) != i || seL4_GetMR(2) != LUNA_RESOURCE_SLOTS) {
            for (;;) seL4_Yield();
        }
        resources[i].tcb = (seL4_CPtr)seL4_GetMR(3);
        resources[i].stack_top = seL4_GetMR(4);
        resources[i].stack_pages = seL4_GetMR(5);
        resources[i].ipc_buffer_addr = seL4_GetMR(6);
        resources[i].join_ntfn = (seL4_CPtr)seL4_GetMR(7);
    }
    struct luna_task_sync_resource sync[LUNA_SYNC_SLOTS];
    for (seL4_Word i = 0; i < LUNA_SYNC_SLOTS; i++) {
        seL4_MessageInfo_t config_tag =
            receive_command(child_boot.command_ep,
                            LUNA_COMMAND_CONFIGURE_SYNC);
        if (seL4_MessageInfo_get_length(config_tag) != 4 ||
            seL4_GetMR(1) != i || seL4_GetMR(2) != LUNA_SYNC_SLOTS) {
            for (;;) seL4_Yield();
        }
        sync[i].ntfn = (seL4_CPtr)seL4_GetMR(3);
    }
    seL4_MessageInfo_t console_tag =
        receive_command(child_boot.command_ep,
                        LUNA_COMMAND_CONFIGURE_CONSOLE);
    if (seL4_MessageInfo_get_length(console_tag) != 2 || !seL4_GetMR(1))
        for (;;) seL4_Yield();
    seL4_CPtr console_io_port = (seL4_CPtr)seL4_GetMR(1);
    seL4_MessageInfo_t disk_tag =
        receive_command(child_boot.command_ep,
                        LUNA_COMMAND_CONFIGURE_DISK);
    if (seL4_MessageInfo_get_length(disk_tag) != 4 ||
        seL4_GetMR(1) != LUNA_DISK_IO_BASE ||
        seL4_GetMR(2) != LUNA_DISK_IO_SIZE ||
        seL4_GetMR(3) != LUNA_PERSISTENT_DISK_SIZE)
        for (;;) seL4_Yield();
    void *disk_io_base = (void *)(uintptr_t)seL4_GetMR(1);
    unsigned long disk_io_size = seL4_GetMR(2);
    unsigned long disk_size = seL4_GetMR(3);
    seL4_MessageInfo_t net_tag =
        receive_command(child_boot.command_ep,
                        LUNA_COMMAND_CONFIGURE_NET);
    if (seL4_MessageInfo_get_length(net_tag) != 6 ||
        seL4_GetMR(1) != LUNA_NET_IO_BASE ||
        seL4_GetMR(2) != LUNA_NET_IO_SIZE ||
        seL4_GetMR(3) != LUNA_NET_MAC_WORD0 ||
        seL4_GetMR(4) != LUNA_NET_MAC_WORD1 || !seL4_GetMR(5))
        for (;;) seL4_Yield();
    void *net_io_base = (void *)(uintptr_t)seL4_GetMR(1);
    unsigned long net_io_size = seL4_GetMR(2);
    seL4_CPtr net_rx_ntfn = (seL4_CPtr)seL4_GetMR(5);

    if (luna_lkl_task_configure_resources(
            resources, sync, console_io_port, child_boot.control_ep,
            child_boot.command_ep))
        for (;;) seL4_Yield();
    if (luna_lkl_task_configure_disk(disk_io_base, disk_io_size, disk_size))
        for (;;) seL4_Yield();
    if (luna_lkl_task_configure_net(net_io_base, net_io_size,
                                    LUNA_NET_MAC_WORD0,
                                    LUNA_NET_MAC_WORD1,
                                    net_rx_ntfn))
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_RESOURCE_CONFIGURED,
               child_boot.mode);

    seL4_MessageInfo_t start_tag =
        receive_command(child_boot.command_ep, LUNA_COMMAND_START);
    if (seL4_MessageInfo_get_length(start_tag) != 2 || !seL4_GetMR(1))
        for (;;) seL4_Yield();
    unsigned long long tsc_frequency = seL4_GetMR(1);
    if (luna_lkl_task_prepare_time(tsc_frequency))
        for (;;) seL4_Yield();

    if (luna_lkl_task_thread_test())
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_RESOURCE_OK,
               child_boot.mode);
    if (luna_lkl_task_allocator_test())
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_ALLOCATOR_OK,
               child_boot.mode);
    if (luna_lkl_task_sync_tls_test())
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_SYNC_TLS_OK,
               child_boot.mode);
    if (luna_lkl_task_thread_timer_test())
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_THREAD_TIMER_OK,
               child_boot.mode);

    if (luna_lkl_task_init()) {
        for (;;) seL4_Yield();
    }
    if ((child_boot.mode != LUNA_ISOLATION_MODE_STRESS &&
         luna_lkl_task_net_add()) || luna_lkl_task_disk_add()) {
        for (;;) seL4_Yield();
    }
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_LKL_INIT_OK,
               child_boot.mode);
    if (luna_lkl_task_start_kernel(tsc_frequency)) {
        for (;;) seL4_Yield();
    }
    send_event(child_boot.control_ep, LUNA_ISOLATION_EVENT_LKL_BOOT_OK,
               child_boot.mode);
    if (child_boot.mode != LUNA_ISOLATION_MODE_STRESS) {
        if (luna_lkl_task_net_prepare())
            for (;;) seL4_Yield();
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_VIRTIO_NET_OK, child_boot.mode);
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_NETWORK_IPV4_OK, child_boot.mode);
    }
    if (luna_lkl_task_disk_prepare(child_boot.mode))
        for (;;) seL4_Yield();
    send_event(child_boot.control_ep,
               LUNA_ISOLATION_EVENT_VIRTIO_BLOCK_OK, child_boot.mode);
    if (child_boot.mode == LUNA_ISOLATION_MODE_CLEAN) {
        if (luna_lkl_task_net_smoke())
            for (;;) seL4_Yield();
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_NETWORK_ICMP_OK, child_boot.mode);
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_NETWORK_TCP_OK, child_boot.mode);
        if (luna_lkl_task_net_pressure_smoke())
            for (;;) seL4_Yield();
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_NETWORK_PRESSURE_OK,
                   child_boot.mode);
        if (luna_lkl_task_net_tx_pressure_smoke())
            for (;;) seL4_Yield();
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_NETWORK_TX_PRESSURE_OK,
                   child_boot.mode);
        if (luna_shell_prepare(luna_lkl_task_console_ready()))
            for (;;) seL4_Yield();
        if (luna_lkl_task_user_smoke())
            for (;;) seL4_Yield();
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_USER_PROGRAM_OK, child_boot.mode);
        send_event(child_boot.control_ep,
                   LUNA_ISOLATION_EVENT_LKL_SHELL_READY, child_boot.mode);
        if (luna_lkl_task_user_shell())
            for (;;) seL4_Yield();
    }
    if (luna_lkl_task_disk_finish() || luna_lkl_task_net_finish())
        for (;;) seL4_Yield();
    luna_lkl_task_console_stop();
    if (luna_lkl_task_halt()) {
        for (;;) seL4_Yield();
    }
    if (luna_lkl_task_disk_cleanup_after_halt())
        for (;;) seL4_Yield();
    if (!luna_lkl_task_allocator_idle())
        for (;;) seL4_Yield();
    if (!luna_lkl_task_sync_tls_runtime_ok())
        for (;;) seL4_Yield();
    finish_child();
}
