/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Own an LKL child with its own CSpace/VSpace, diagnose an intentional fault,
 * destroy it, and start a clean replacement without exposing manager
 * allocation authority to the child.
 */
#include "luna_isolation.h"
#include "luna_isolation_protocol.h"
#include "luna_network_manager.h"

#include <sel4/sel4.h>
#include <sel4utils/process.h>
#include <sel4utils/process_config.h>
#include <sel4utils/thread.h>
#include <sel4platsupport/arch/io.h>
#include <sel4platsupport/io.h>
#include <platsupport/io.h>
#include <vka/object.h>
#include <vka/capops.h>
#include <cpio/cpio.h>
#include <utils/util.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define CHILD_PRIORITY 100
#define CHILD_BADGE_FAULT 0x41
#define CHILD_BADGE_CLEAN 0x42
#define CHILD_BADGE_STRESS 0x43
#define MANAGER_PRIVATE_ADDR ((void *)(uintptr_t)0x40000000UL)
#define MANAGER_DISK_IO_ADDR ((void *)(uintptr_t)0x52000000UL)
#define LUNA_DISK_PCI_BUS 0U
#define LUNA_DISK_PCI_DEVICE 6U
#define LUNA_DISK_PCI_FUNCTION 0U
#define LUNA_DISK_PCI_VENDOR 0x1af4U
#define LUNA_DISK_PCI_DEVICE_ID 0x1110U
#define PCI_CONFIG_ADDRESS 0x0cf8U
#define PCI_CONFIG_DATA 0x0cfcU
#define PCI_COMMAND 0x04U
#define PCI_BAR2 0x18U
#define PCI_COMMAND_MEMORY 0x2U

extern char _cpio_archive[];
extern char _cpio_archive_end[];

struct luna_resource_slot {
    sel4utils_thread_t thread;
    vka_object_t join_ntfn;
    seL4_CPtr child_tcb;
    seL4_CPtr child_join_ntfn;
    int thread_configured;
    int join_allocated;
};

struct luna_sync_slot {
    vka_object_t ntfn;
    seL4_CPtr child_ntfn;
    int allocated;
};

struct luna_console_resource {
    cspacepath_t io_port_path;
    seL4_CPtr child_io_port;
    int slot_allocated;
    int cap_created;
};

struct luna_heap_resource {
    reservation_t reservation;
    unsigned char mapped[LUNA_CHILD_HEAP_PAGES];
    size_t mapped_pages;
    size_t peak_pages;
    int reserved;
};

struct luna_disk_mapping {
    reservation_t reservation;
    cspacepath_t frame_caps[LUNA_DISK_IO_PAGES];
    unsigned char cap_allocated[LUNA_DISK_IO_PAGES];
    size_t mapped_pages;
    int reserved;
};

struct luna_persistent_disk {
    ps_io_ops_t io_ops;
    void *mapping;
    size_t bytes;
    int allocated;
    void *io_mapping;
    reservation_t io_reservation;
    int io_reserved;
    int io_allocated;
};

struct luna_child_resources {
    struct luna_resource_slot slots[LUNA_RESOURCE_SLOTS];
    struct luna_sync_slot sync[LUNA_SYNC_SLOTS];
    struct luna_console_resource console;
    struct luna_heap_resource heap;
    struct luna_disk_mapping disk;
    struct luna_net_mapping net;
};

struct luna_event_context {
    seL4_CPtr control_ep;
    seL4_CPtr command_ep;
    seL4_Word badge;
    sel4utils_process_t *process;
    struct luna_child_resources *resources;
    struct luna_persistent_disk *disk;
    vka_t *vka;
};

static unsigned long long manager_elapsed_ns(unsigned long long start_cycles,
                                             unsigned long long frequency)
{
    unsigned long long cycles = __builtin_ia32_rdtsc() - start_cycles;
    if (!frequency) return 0;
    return (cycles / frequency) * 1000000000ULL +
           ((cycles % frequency) * 1000000000ULL) / frequency;
}

static int delete_child_cap(sel4utils_process_t *process, seL4_CPtr cap)
{
    if (!cap || !process->cspace.cptr) return 0;
    cspacepath_t path = {
        .root = process->cspace.cptr,
        .capPtr = cap,
        .capDepth = process->cspace_size,
    };
    int error = vka_cnode_delete(&path);
    if (error)
        printf("luna: failed to delete child resource cap %lu: %d\n", cap, error);
    return error;
}

static uint32_t disk_pci_config_key(unsigned offset)
{
    return 0x80000000U | (LUNA_DISK_PCI_BUS << 16) |
           (LUNA_DISK_PCI_DEVICE << 11) |
           (LUNA_DISK_PCI_FUNCTION << 8) | (offset & ~3U);
}

static int disk_pci_read(struct luna_persistent_disk *disk,
                         unsigned offset, int size, uint32_t *value)
{
    if (ps_io_port_out(&disk->io_ops.io_port_ops, PCI_CONFIG_ADDRESS,
                       4, disk_pci_config_key(offset)))
        return -1;
    return ps_io_port_in(&disk->io_ops.io_port_ops,
                         PCI_CONFIG_DATA + (offset & 3U), size, value);
}

static int disk_pci_write(struct luna_persistent_disk *disk,
                          unsigned offset, int size, uint32_t value)
{
    if (ps_io_port_out(&disk->io_ops.io_port_ops, PCI_CONFIG_ADDRESS,
                       4, disk_pci_config_key(offset)))
        return -1;
    return ps_io_port_out(&disk->io_ops.io_port_ops,
                          PCI_CONFIG_DATA + (offset & 3U), size, value);
}

static int init_persistent_disk(struct luna_persistent_disk *disk,
                                simple_t *simple, vka_t *vka,
                                vspace_t *manager_vspace)
{
    uint32_t vendor = 0, device = 0, bar_low = 0, bar_high = 0;
    uint32_t command = 0;
    if (sel4platsupport_new_io_ops(manager_vspace, vka, simple,
                                   &disk->io_ops) ||
        sel4platsupport_new_arch_ops(&disk->io_ops, simple, vka)) {
        printf("luna: host-file disk I/O setup failed\n");
        return -1;
    }
    if (disk_pci_read(disk, 0x00, 2, &vendor) ||
        disk_pci_read(disk, 0x02, 2, &device) ||
        vendor != LUNA_DISK_PCI_VENDOR ||
        device != LUNA_DISK_PCI_DEVICE_ID) {
        printf("luna: ivshmem disk missing at 00:06.0 "
               "vendor=%04x device=%04x\n", vendor, device);
        return -1;
    }
    if (disk_pci_read(disk, PCI_BAR2, 4, &bar_low) ||
        disk_pci_read(disk, PCI_BAR2 + 4, 4, &bar_high) ||
        (bar_low & 1U) || (bar_low & 6U) != 4U ||
        disk_pci_read(disk, PCI_COMMAND, 2, &command) ||
        disk_pci_write(disk, PCI_COMMAND, 2,
                       command | PCI_COMMAND_MEMORY)) {
        printf("luna: ivshmem disk PCI configuration invalid "
               "bar=%08x:%08x\n", bar_high, bar_low);
        return -1;
    }
    uint64_t physical = ((uint64_t)bar_high << 32) |
                        (uint64_t)(bar_low & ~0xfU);
    if (!physical || physical % LUNA_PERSISTENT_DISK_SIZE) {
        printf("luna: ivshmem disk BAR alignment invalid: %llx\n",
               (unsigned long long)physical);
        return -1;
    }
    disk->mapping = ps_io_map(&disk->io_ops.io_mapper, (uintptr_t)physical,
                              LUNA_PERSISTENT_DISK_SIZE, 1,
                              PS_MEM_NORMAL);
    if (!disk->mapping) {
        printf("luna: ivshmem host-file disk mapping failed: %llx\n",
               (unsigned long long)physical);
        return -1;
    }
    disk->allocated = 1;
    disk->bytes = LUNA_PERSISTENT_DISK_SIZE;
    disk->io_reservation = vspace_reserve_range_at(
        manager_vspace, MANAGER_DISK_IO_ADDR, LUNA_DISK_IO_SIZE,
        seL4_AllRights, 1);
    if (!disk->io_reservation.res) {
        printf("luna: persistent disk I/O reservation failed\n");
        return -1;
    }
    disk->io_reserved = 1;
    if (vspace_new_pages_at_vaddr(
            manager_vspace, MANAGER_DISK_IO_ADDR, LUNA_DISK_IO_PAGES,
            seL4_PageBits, disk->io_reservation)) {
        printf("luna: persistent disk I/O window allocation failed\n");
        return -1;
    }
    disk->io_mapping = MANAGER_DISK_IO_ADDR;
    disk->io_allocated = 1;
    memset(disk->io_mapping, 0, LUNA_DISK_IO_SIZE);
    printf("luna: host-file ext4 backing ready PCI=00:06.0 "
           "paddr=%llx bytes=%zu\n",
           (unsigned long long)physical, disk->bytes);
    return 0;
}

static int map_persistent_disk(struct luna_persistent_disk *disk,
                               vka_t *vka, vspace_t *manager_vspace,
                               sel4utils_process_t *process,
                               struct luna_child_resources *resources)
{
    struct luna_disk_mapping *mapping = &resources->disk;
    if (!disk->allocated || !disk->io_allocated ||
        disk->bytes != LUNA_PERSISTENT_DISK_SIZE)
        return -1;

    mapping->reservation = vspace_reserve_range_at(
        &process->vspace, (void *)(uintptr_t)LUNA_DISK_IO_BASE,
        LUNA_DISK_IO_SIZE, seL4_AllRights, 1);
    if (!mapping->reservation.res) {
        printf("luna: child persistent disk reservation failed\n");
        return -1;
    }
    mapping->reserved = 1;

    for (size_t i = 0; i < LUNA_DISK_IO_PAGES; i++) {
        void *source_address = (void *)((uintptr_t)disk->io_mapping +
            i * BIT(seL4_PageBits));
        seL4_CPtr source_cap = vspace_get_cap(manager_vspace,
                                               source_address);
        if (!source_cap ||
            vka_cspace_alloc_path(vka, &mapping->frame_caps[i])) {
            printf("luna: persistent disk frame slot %zu allocation failed\n",
                   i);
            return -1;
        }
        cspacepath_t source_path;
        vka_cspace_make_path(vka, source_cap, &source_path);
        if (vka_cnode_copy(&mapping->frame_caps[i], &source_path,
                           seL4_AllRights)) {
            printf("luna: persistent disk frame %zu copy failed\n", i);
            vka_cspace_free_path(vka, mapping->frame_caps[i]);
            return -1;
        }
        mapping->cap_allocated[i] = 1;
        void *child_address = (void *)(uintptr_t)(
            LUNA_DISK_IO_BASE + i * BIT(seL4_PageBits));
        seL4_CPtr cap = mapping->frame_caps[i].capPtr;
        if (vspace_map_pages_at_vaddr(
                &process->vspace, &cap, NULL, child_address, 1,
                seL4_PageBits, mapping->reservation)) {
            printf("luna: persistent disk frame %zu map failed\n", i);
            return -1;
        }
        mapping->mapped_pages++;
    }
    return 0;
}

static int destroy_child(sel4utils_process_t *process,
                         struct luna_child_resources *resources, vka_t *vka)
{
    int result = 0;
    size_t disk_mapped_pages = resources->disk.mapped_pages;
    size_t net_mapped_pages = resources->net.mapped_pages;
    luna_network_deactivate_child();
    if (process->thread.tcb.cptr)
        seL4_TCB_Suspend(process->thread.tcb.cptr);
    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct luna_resource_slot *slot = &resources->slots[i];
        if (slot->thread_configured)
            seL4_TCB_Suspend(slot->thread.tcb.cptr);

        /* Delete foreign CSpace copies before returning their backing objects
         * to the Untyped allocator. Otherwise the allocator can offer a range
         * that the kernel still considers occupied by a derived cap. */
        if (delete_child_cap(process, slot->child_tcb)) result = -1;
        if (delete_child_cap(process, slot->child_join_ntfn)) result = -1;

        if (slot->thread_configured)
            sel4utils_clean_up_thread(vka, &process->vspace, &slot->thread);
        if (slot->join_allocated)
            vka_free_object(vka, &slot->join_ntfn);
    }
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        struct luna_sync_slot *slot = &resources->sync[i];
        if (delete_child_cap(process, slot->child_ntfn)) result = -1;
        if (slot->allocated)
            vka_free_object(vka, &slot->ntfn);
    }
    if (delete_child_cap(process, resources->console.child_io_port)) result = -1;
    if (resources->console.cap_created &&
        vka_cnode_delete(&resources->console.io_port_path))
        result = -1;
    if (resources->console.slot_allocated)
        vka_cspace_free_path(vka, resources->console.io_port_path);
    if (resources->heap.reserved) {
        for (size_t i = 0; i < LUNA_CHILD_HEAP_PAGES;) {
            if (!resources->heap.mapped[i]) {
                i++;
                continue;
            }
            size_t start = i;
            while (i < LUNA_CHILD_HEAP_PAGES &&
                   resources->heap.mapped[i])
                i++;
            vspace_unmap_pages(
                &process->vspace,
                (void *)(uintptr_t)(LUNA_CHILD_HEAP_BASE +
                                    start * BIT(seL4_PageBits)),
                i - start, seL4_PageBits, vka);
        }
        vspace_free_reservation(&process->vspace,
                                resources->heap.reservation);
    }
    if (process->cspace.cptr)
        sel4utils_destroy_process(process, vka);
    /* Mapped duplicate frame caps are owned by the child VSpace teardown.
     * Only a cap copied immediately before a failed map is absent from that
     * bookkeeping and must be deleted explicitly here. */
    for (size_t i = disk_mapped_pages;
         i < LUNA_DISK_IO_PAGES; i++) {
        if (!resources->disk.cap_allocated[i]) continue;
        if (vka_cnode_delete(&resources->disk.frame_caps[i])) result = -1;
        vka_cspace_free_path(vka, resources->disk.frame_caps[i]);
    }
    for (size_t i = net_mapped_pages; i < LUNA_NET_IO_PAGES; i++) {
        if (!resources->net.cap_allocated[i]) continue;
        if (vka_cnode_delete(&resources->net.frame_caps[i])) result = -1;
        vka_cspace_free_path(vka, resources->net.frame_caps[i]);
    }
    memset(resources, 0, sizeof(*resources));
    memset(process, 0, sizeof(*process));
    return result;
}

static int configure_resource_pool(simple_t *simple, vka_t *vka,
                                   vspace_t *manager_vspace,
                                   sel4utils_process_t *process,
                                   struct luna_child_resources *resources)
{
    seL4_Word cspace_data =
        api_make_guard_skip_word(seL4_WordBits - process->cspace_size);

    for (int i = 0; i < LUNA_RESOURCE_SLOTS; i++) {
        struct luna_resource_slot *slot = &resources->slots[i];
        sel4utils_thread_config_t config = {0};
        config = thread_config_cspace(config, process->cspace.cptr, cspace_data);
        config = thread_config_fault_endpoint(config, SEL4UTILS_ENDPOINT_SLOT);
        config = thread_config_auth(config, simple_get_tcb(simple));
        config = thread_config_priority(config, CHILD_PRIORITY);
        config = thread_config_stack_size(config, LUNA_RESOURCE_STACK_PAGES);
        config = thread_config_create_reply(config);

        if (sel4utils_configure_thread_config(vka, manager_vspace,
                                              &process->vspace, config,
                                              &slot->thread)) {
            printf("luna: resource slot %d thread configure failed\n", i);
            return -1;
        }
        slot->thread_configured = 1;

        slot->child_tcb = sel4utils_copy_cap_to_process(
            process, vka, slot->thread.tcb.cptr);
        if (!slot->child_tcb) {
            printf("luna: resource slot %d TCB cap copy failed\n", i);
            return -1;
        }

        if (vka_alloc_notification(vka, &slot->join_ntfn)) {
            printf("luna: resource slot %d join allocation failed\n", i);
            return -1;
        }
        slot->join_allocated = 1;
        slot->child_join_ntfn = sel4utils_copy_cap_to_process(
            process, vka, slot->join_ntfn.cptr);
        if (!slot->child_join_ntfn) {
            printf("luna: resource slot %d join cap copy failed\n", i);
            return -1;
        }
    }
    for (int i = 0; i < LUNA_SYNC_SLOTS; i++) {
        struct luna_sync_slot *slot = &resources->sync[i];
        if (vka_alloc_notification(vka, &slot->ntfn)) {
            printf("luna: sync slot %d allocation failed\n", i);
            return -1;
        }
        slot->allocated = 1;
        slot->child_ntfn = sel4utils_copy_cap_to_process(
            process, vka, slot->ntfn.cptr);
        if (!slot->child_ntfn) {
            printf("luna: sync slot %d cap copy failed\n", i);
            return -1;
        }
    }
    struct luna_console_resource *console = &resources->console;
    if (vka_cspace_alloc_path(vka, &console->io_port_path)) {
        printf("luna: console I/O cap slot allocation failed\n");
        return -1;
    }
    console->slot_allocated = 1;
    if (simple_get_IOPort_cap(simple, 0x3f8, 0x3ff,
                              console->io_port_path.root,
                              console->io_port_path.capPtr,
                              console->io_port_path.capDepth)) {
        printf("luna: COM1 I/O cap allocation failed\n");
        return -1;
    }
    console->cap_created = 1;
    console->child_io_port = sel4utils_copy_cap_to_process(
        process, vka, console->io_port_path.capPtr);
    if (!console->child_io_port) {
        printf("luna: COM1 I/O cap copy failed\n");
        return -1;
    }
    return 0;
}

static int service_memory_request(struct luna_event_context *context,
                                  seL4_Word event, seL4_Word address,
                                  seL4_Word pages)
{
    struct luna_heap_resource *heap = &context->resources->heap;
    int error = 0;
    uintptr_t base = LUNA_CHILD_HEAP_BASE;
    uintptr_t end = base + LUNA_CHILD_HEAP_SIZE;
    uintptr_t request = (uintptr_t)address;
    size_t page_count = (size_t)pages;
    if (!heap->reserved || !page_count || request < base || request >= end ||
        (request & (BIT(seL4_PageBits) - 1)) ||
        page_count > (end - request) / BIT(seL4_PageBits)) {
        error = -1;
    }

    size_t first_page = error ? 0 :
        (request - base) / BIT(seL4_PageBits);
    if (!error) {
        for (size_t i = 0; i < page_count; i++) {
            int mapped = heap->mapped[first_page + i] != 0;
            if ((event == LUNA_ISOLATION_EVENT_MEMORY_MAP && mapped) ||
                (event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP && !mapped)) {
                error = -1;
                break;
            }
        }
    }

    if (!error && event == LUNA_ISOLATION_EVENT_MEMORY_MAP) {
        error = vspace_new_pages_at_vaddr(
            &context->process->vspace, (void *)request, page_count,
            seL4_PageBits, heap->reservation);
        if (!error) {
            memset(&heap->mapped[first_page], 1, page_count);
            heap->mapped_pages += page_count;
            if (heap->mapped_pages > heap->peak_pages)
                heap->peak_pages = heap->mapped_pages;
        }
    } else if (!error && event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP) {
        vspace_unmap_pages(&context->process->vspace, (void *)request,
                           page_count, seL4_PageBits, context->vka);
        memset(&heap->mapped[first_page], 0, page_count);
        heap->mapped_pages -= page_count;
    } else if (!error) {
        error = -1;
    }

    seL4_SetMR(0, LUNA_COMMAND_MEMORY_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_Send(context->command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    if (error)
        printf("luna: child heap request failed event=%lu addr=%p pages=%lu\n",
               event, (void *)request, pages);
    return error;
}

static int service_disk_request(struct luna_event_context *context,
                                seL4_Word event, seL4_Word offset_word,
                                seL4_Word length_word)
{
    struct luna_persistent_disk *disk = context->disk;
    size_t offset = (size_t)offset_word;
    size_t length = (size_t)length_word;
    int error = 0;
    if (!disk || !disk->allocated || !disk->io_allocated ||
        offset > disk->bytes ||
        length > disk->bytes - offset ||
        length > LUNA_DISK_IO_SIZE) {
        error = -1;
    } else if (event == LUNA_ISOLATION_EVENT_DISK_READ) {
        if (!length) error = -1;
        else memcpy(disk->io_mapping,
                    (unsigned char *)disk->mapping + offset,
                    length);
    } else if (event == LUNA_ISOLATION_EVENT_DISK_WRITE) {
        if (!length) error = -1;
        else memcpy((unsigned char *)disk->mapping + offset,
                    disk->io_mapping, length);
    } else if (event != LUNA_ISOLATION_EVENT_DISK_FLUSH || length) {
        error = -1;
    }
    __sync_synchronize();
    seL4_SetMR(0, LUNA_COMMAND_DISK_RESULT);
    seL4_SetMR(1, (seL4_Word)error);
    seL4_Send(context->command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
    if (error)
        printf("luna: child disk request failed event=%lu offset=%lu "
               "length=%lu allocated=%d io=%d bytes=%zu window=%lu\n",
               event, offset_word, length_word, disk ? disk->allocated : 0,
               disk ? disk->io_allocated : 0, disk ? disk->bytes : 0,
               (unsigned long)LUNA_DISK_IO_SIZE);
    return error;
}

static int receive_event(struct luna_event_context *context,
                         seL4_Word expected_event,
                         seL4_Word expected_detail)
{
    for (;;) {
        seL4_Word badge = 0;
        seL4_MessageInfo_t tag = seL4_Recv(context->control_ep, &badge);
        seL4_Word label = seL4_MessageInfo_get_label(tag);
        seL4_Word length = seL4_MessageInfo_get_length(tag);
        seL4_Word event = length > 0 ? seL4_GetMR(0) : 0;
        seL4_Word detail = length > 1 ? seL4_GetMR(1) : 0;

        if (label == 0 && length == 3 && badge == context->badge &&
            (event == LUNA_ISOLATION_EVENT_MEMORY_MAP ||
             event == LUNA_ISOLATION_EVENT_MEMORY_UNMAP)) {
            if (service_memory_request(context, event, seL4_GetMR(1),
                                       seL4_GetMR(2)))
                return -1;
            continue;
        }

        if (label == 0 && length == 3 && badge == context->badge &&
            (event == LUNA_ISOLATION_EVENT_DISK_READ ||
             event == LUNA_ISOLATION_EVENT_DISK_WRITE ||
             event == LUNA_ISOLATION_EVENT_DISK_FLUSH)) {
            if (service_disk_request(context, event, seL4_GetMR(1),
                                     seL4_GetMR(2)))
                return -1;
            continue;
        }

        if (label == 0 && length == 3 && badge == context->badge &&
            (event == LUNA_ISOLATION_EVENT_NET_TX ||
             event == LUNA_ISOLATION_EVENT_NET_RX ||
             event == LUNA_ISOLATION_EVENT_NET_WAKE ||
             event == LUNA_ISOLATION_EVENT_NET_CONTROL ||
             event == LUNA_ISOLATION_EVENT_NET_STATS ||
             event == LUNA_ISOLATION_EVENT_NET_TX_STATS ||
             event == LUNA_ISOLATION_EVENT_NET_TX_STRESS)) {
            if (seL4_GetMR(2) ||
                luna_network_service(context->command_ep, event,
                                     seL4_GetMR(1)))
                return -1;
            continue;
        }

        if (label != 0 || length < 2 || badge != context->badge ||
            event != expected_event || detail != expected_detail) {
            printf("luna: isolation event mismatch label=%lu len=%lu badge=%lu "
                   "event=%lu detail=%lu\n",
                   label, length, badge, event, detail);
            return -1;
        }
        if ((event == LUNA_ISOLATION_EVENT_ALLOCATOR_OK ||
             event == LUNA_ISOLATION_EVENT_ALLOCATOR_RELEASED) &&
            context->resources->heap.mapped_pages != 0) {
            printf("luna: child heap retained %zu pages at event %lu\n",
                   context->resources->heap.mapped_pages, event);
            return -1;
        }
        if (event == LUNA_ISOLATION_EVENT_ALLOCATOR_OK &&
            context->resources->heap.peak_pages !=
                LUNA_CHILD_HEAP_PAGES) {
            printf("luna: child heap peak was %zu pages, expected %lu\n",
                   context->resources->heap.peak_pages,
                   (unsigned long)LUNA_CHILD_HEAP_PAGES);
            return -1;
        }
        return 0;
    }
}

static void send_resource_slot(seL4_CPtr command_ep,
                               const struct luna_resource_slot *slot,
                               seL4_Word index)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_SLOT);
    seL4_SetMR(1, index);
    seL4_SetMR(2, LUNA_RESOURCE_SLOTS);
    seL4_SetMR(3, slot->child_tcb);
    seL4_SetMR(4, (seL4_Word)(uintptr_t)slot->thread.stack_top);
    seL4_SetMR(5, slot->thread.stack_size);
    seL4_SetMR(6, slot->thread.ipc_buffer_addr);
    seL4_SetMR(7, slot->child_join_ntfn);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 8));
}

static void send_start(seL4_CPtr command_ep, unsigned long long tsc_frequency)
{
    seL4_SetMR(0, LUNA_COMMAND_START);
    seL4_SetMR(1, (seL4_Word)tsc_frequency);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void send_sync_slot(seL4_CPtr command_ep,
                           const struct luna_sync_slot *slot,
                           seL4_Word index)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_SYNC);
    seL4_SetMR(1, index);
    seL4_SetMR(2, LUNA_SYNC_SLOTS);
    seL4_SetMR(3, slot->child_ntfn);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 4));
}

static void send_console_resource(
    seL4_CPtr command_ep, const struct luna_console_resource *console)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_CONSOLE);
    seL4_SetMR(1, console->child_io_port);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 2));
}

static void send_disk_resource(seL4_CPtr command_ep)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_DISK);
    seL4_SetMR(1, LUNA_DISK_IO_BASE);
    seL4_SetMR(2, LUNA_DISK_IO_SIZE);
    seL4_SetMR(3, LUNA_PERSISTENT_DISK_SIZE);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 4));
}

static void send_net_resource(seL4_CPtr command_ep,
                              const struct luna_net_mapping *mapping)
{
    seL4_SetMR(0, LUNA_COMMAND_CONFIGURE_NET);
    seL4_SetMR(1, LUNA_NET_IO_BASE);
    seL4_SetMR(2, LUNA_NET_IO_SIZE);
    seL4_SetMR(3, LUNA_NET_MAC_WORD0);
    seL4_SetMR(4, LUNA_NET_MAC_WORD1);
    seL4_SetMR(5, mapping->child_rx_ntfn);
    seL4_Send(command_ep, seL4_MessageInfo_new(0, 0, 0, 6));
}

static int start_child(simple_t *simple, vka_t *vka, vspace_t *manager_vspace,
                       seL4_CPtr control_ep, seL4_CPtr command_ep,
                       seL4_Word badge,
                       enum luna_isolation_mode mode, seL4_Word private_addr,
                       sel4utils_process_t *process,
                       struct luna_child_resources *resources,
                       struct luna_persistent_disk *disk)
{
    sel4utils_process_config_t config =
        process_config_default_simple(simple, LUNA_ISOLATION_CHILD_IMAGE, CHILD_PRIORITY);
    if (sel4utils_configure_process_custom(process, vka, manager_vspace, config)) {
        printf("luna: isolation child configure failed\n");
        return -1;
    }

    if (!process->own_cspace || !process->own_vspace || !process->own_ep) {
        printf("luna: isolation child did not receive independent resources\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    if (process->pd.cptr == simple_get_pd(simple) ||
        process->cspace.cptr == simple_get_cnode(simple)) {
        printf("luna: isolation child reused manager address or capability space\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    resources->heap.reservation = vspace_reserve_range_at(
        &process->vspace, (void *)(uintptr_t)LUNA_CHILD_HEAP_BASE,
        LUNA_CHILD_HEAP_SIZE, seL4_AllRights, 1);
    if (!resources->heap.reservation.res) {
        printf("luna: child heap reservation failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    resources->heap.reserved = 1;

    if (map_persistent_disk(disk, vka, manager_vspace, process, resources)) {
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    if (luna_network_map_child(vka, manager_vspace, process,
                               &resources->net,
                               mode != LUNA_ISOLATION_MODE_STRESS)) {
        printf("luna: child network window mapping failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    if (configure_resource_pool(simple, vka, manager_vspace, process, resources)) {
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t control_path;
    vka_cspace_make_path(vka, control_ep, &control_path);
    seL4_CPtr child_control = sel4utils_mint_cap_to_process(
        process, control_path, seL4_CapRights_new(false, false, false, true), badge);
    if (!child_control) {
        printf("luna: isolation control cap mint failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    cspacepath_t command_path;
    vka_cspace_make_path(vka, command_ep, &command_path);
    seL4_CPtr child_command = sel4utils_mint_cap_to_process(
        process, command_path, seL4_CapRights_new(false, false, true, false), 0);
    if (!child_command) {
        printf("luna: isolation command cap mint failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }

    char arg_strings[4][WORD_STRING_SIZE];
    char *argv[4];
    sel4utils_create_word_args(arg_strings, argv, 4,
                               (seL4_Word)child_control,
                               (seL4_Word)child_command,
                               (seL4_Word)mode,
                               private_addr);
    if (sel4utils_spawn_process_v(process, vka, manager_vspace, 4, argv, 1)) {
        printf("luna: isolation child spawn failed\n");
        (void)destroy_child(process, resources, vka);
        return -1;
    }
    return 0;
}

static int boot_child(simple_t *simple, vka_t *vka,
                      vspace_t *manager_vspace, seL4_CPtr control_ep,
                      seL4_CPtr command_ep, seL4_Word badge,
                      enum luna_isolation_mode mode, seL4_Word private_addr,
                      unsigned long long tsc_frequency,
                      sel4utils_process_t *process,
                      struct luna_child_resources *resources,
                      struct luna_persistent_disk *disk,
                      int *child_live,
                      struct luna_event_context *event_context)
{
    unsigned long long create_begin = __builtin_ia32_rdtsc();
    if (start_child(simple, vka, manager_vspace, control_ep, command_ep,
                    badge, mode, private_addr, process, resources, disk))
        return -1;
    if (mode == LUNA_ISOLATION_MODE_STRESS)
        printf("LUNA_LIFECYCLE_CREATE_SAMPLE ns=%llu\n",
               manager_elapsed_ns(create_begin, tsc_frequency));
    *child_live = 1;
    *event_context = (struct luna_event_context) {
        .control_ep = control_ep,
        .command_ep = command_ep,
        .badge = badge,
        .process = process,
        .resources = resources,
        .disk = disk,
        .vka = vka,
    };

    if (receive_event(event_context, LUNA_ISOLATION_EVENT_READY, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_LINKED, mode))
        return -1;

    for (seL4_Word i = 0; i < LUNA_RESOURCE_SLOTS; i++)
        send_resource_slot(command_ep, &resources->slots[i], i);
    for (seL4_Word i = 0; i < LUNA_SYNC_SLOTS; i++)
        send_sync_slot(command_ep, &resources->sync[i], i);
    send_console_resource(command_ep, &resources->console);
    send_disk_resource(command_ep);
    send_net_resource(command_ep, &resources->net);
    if (receive_event(event_context,
                      LUNA_ISOLATION_EVENT_RESOURCE_CONFIGURED, mode))
        return -1;
    unsigned long long start_begin = __builtin_ia32_rdtsc();
    send_start(command_ep, tsc_frequency);

    if (receive_event(event_context, LUNA_ISOLATION_EVENT_RESOURCE_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_ALLOCATOR_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_SYNC_TLS_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_THREAD_TIMER_OK,
                      mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_INIT_OK, mode) ||
        receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_BOOT_OK, mode))
        return -1;
    if (mode != LUNA_ISOLATION_MODE_STRESS &&
        (receive_event(event_context,
                       LUNA_ISOLATION_EVENT_VIRTIO_NET_OK, mode) ||
         receive_event(event_context,
                       LUNA_ISOLATION_EVENT_NETWORK_IPV4_OK, mode)))
        return -1;
    if (receive_event(event_context,
                      LUNA_ISOLATION_EVENT_VIRTIO_BLOCK_OK, mode))
        return -1;
    if (mode == LUNA_ISOLATION_MODE_STRESS)
        printf("LUNA_LIFECYCLE_START_SAMPLE ns=%llu\n",
               manager_elapsed_ns(start_begin, tsc_frequency));
    return 0;
}

static int wait_child_halt(struct luna_event_context *event_context,
                           enum luna_isolation_mode mode)
{
    if (receive_event(event_context, LUNA_ISOLATION_EVENT_LKL_HALT_OK, mode) ||
        receive_event(event_context,
                      LUNA_ISOLATION_EVENT_ALLOCATOR_RELEASED, mode))
        return -1;
    return 0;
}

static int release_child(sel4utils_process_t *child,
                         struct luna_child_resources *resources,
                         vka_t *vka, int *child_live)
{
    int error = destroy_child(child, resources, vka);
    *child_live = 0;
    return error;
}

int luna_isolation_smoke(simple_t *simple, vka_t *vka,
                         vspace_t *manager_vspace,
                         unsigned long long tsc_frequency)
{
    reservation_t private_reservation = vspace_reserve_range_at(
        manager_vspace, MANAGER_PRIVATE_ADDR, BIT(seL4_PageBits),
        seL4_AllRights, 1);
    if (!private_reservation.res) {
        printf("luna: manager-private reservation failed\n");
        return -1;
    }
    if (vspace_new_pages_at_vaddr(manager_vspace, MANAGER_PRIVATE_ADDR, 1,
                                  seL4_PageBits, private_reservation)) {
        printf("luna: manager-private page allocation failed\n");
        vspace_free_reservation(manager_vspace, private_reservation);
        return -1;
    }
    volatile seL4_Word *manager_private_page = MANAGER_PRIVATE_ADDR;
    const seL4_Word private_addr =
        (seL4_Word)(uintptr_t)manager_private_page;
    vka_object_t control_ep = {0};
    vka_object_t command_ep = {0};
    sel4utils_process_t child = {0};
    struct luna_child_resources resources = {0};
    struct luna_persistent_disk disk = {0};
    struct luna_event_context event_context = {0};
    int control_allocated = 0;
    int command_allocated = 0;
    int child_live = 0;
    int result = -1;

    manager_private_page[0] = LUNA_ISOLATION_SECRET;

    if (luna_network_manager_init(simple, vka, manager_vspace)) goto out;
    if (init_persistent_disk(&disk, simple, vka, manager_vspace)) goto out;

    if (vka_alloc_endpoint(vka, &control_ep)) {
        printf("luna: isolation control endpoint allocation failed\n");
        goto out;
    }
    control_allocated = 1;

    if (vka_alloc_endpoint(vka, &command_ep)) {
        printf("luna: isolation command endpoint allocation failed\n");
        goto out;
    }
    command_allocated = 1;

    if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                   command_ep.cptr, CHILD_BADGE_FAULT,
                   LUNA_ISOLATION_MODE_FAULT, private_addr, tsc_frequency,
                   &child, &resources, &disk, &child_live, &event_context))
        goto out;
    printf("luna: isolation child created with private CSpace/VSpace\n");
    printf("LUNA_LKL_CHILD_LINKED\n");
    printf("LUNA_RESOURCE_POOL_OK\n");
    printf("LUNA_CHILD_ALLOCATOR_OK pages=%lu\n",
           (unsigned long)LUNA_CHILD_HEAP_PAGES);
    printf("LUNA_SYNC_TLS_OK\n");
    printf("LUNA_THREAD_TIMER_OK\n");
    printf("LUNA_LKL_CHILD_INIT_OK\n");
    printf("LUNA_LKL_CHILD_BOOT_OK\n");
    printf("LUNA_VIRTIO_BLOCK_OK bytes=%lu\n",
           (unsigned long)LUNA_PERSISTENT_DISK_SIZE);
    printf("LUNA_HOST_FILE_BACKING_OK bytes=%lu\n",
           (unsigned long)LUNA_PERSISTENT_DISK_SIZE);
    printf("LUNA_VIRTIO_NET_OK backend=qemu-virtio-pci\n");
    printf("LUNA_NETWORK_IPV4_OK address=10.0.2.15/24\n");
    printf("LUNA_NETWORK_ASYNC_RX_OK notification=receive-only\n");
    printf("LUNA_ROOT_ALLOCATOR_POOL_OK bytes=%lu\n",
           (unsigned long)LUNA_ROOT_ALLOCATOR_POOL_SIZE);
    printf("LUNA_RESOURCE_PEAK_OK managed_frame_pages=%zu child_tcbs=%lu "
           "child_notifications=%lu heap_pages=%zu disk_pages=%zu "
           "net_pages=%zu\n",
           resources.heap.peak_pages + resources.disk.mapped_pages +
               resources.net.mapped_pages,
           (unsigned long)(LUNA_RESOURCE_SLOTS + 1),
           (unsigned long)(LUNA_RESOURCE_SLOTS + LUNA_SYNC_SLOTS + 1),
           resources.heap.peak_pages, resources.disk.mapped_pages,
           resources.net.mapped_pages);
    if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_FAULT))
        goto out;
    printf("LUNA_LKL_CHILD_HALT_OK\n");
    printf("LUNA_CHILD_ALLOCATOR_RELEASE_OK\n");

    seL4_Word fault_badge = 0;
    seL4_MessageInfo_t fault_tag = seL4_Recv(child.fault_endpoint.cptr, &fault_badge);
    seL4_Fault_t fault = seL4_getFault(fault_tag);
    if (seL4_Fault_get_seL4_FaultType(fault) != seL4_Fault_VMFault ||
        seL4_Fault_VMFault_get_Addr(fault) != private_addr) {
        printf("luna: unexpected isolation fault label=%lu addr=%p\n",
               seL4_MessageInfo_get_label(fault_tag),
               (void *)(uintptr_t)seL4_Fault_VMFault_get_Addr(fault));
        goto out;
    }
    sel4utils_print_fault_message(fault_tag, LUNA_ISOLATION_CHILD_IMAGE);
    if (manager_private_page[0] != LUNA_ISOLATION_SECRET) {
        printf("luna: manager-private page was modified\n");
        goto out;
    }
    printf("LUNA_ISOLATION_FAULT_OK addr=%p\n", (void *)(uintptr_t)private_addr);

    if (release_child(&child, &resources, vka, &child_live)) goto out;

    for (int round = 1; round <= LUNA_RESTART_STRESS_ROUNDS; round++) {
        if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                       command_ep.cptr, CHILD_BADGE_STRESS,
                       LUNA_ISOLATION_MODE_STRESS, private_addr,
                       tsc_frequency, &child, &resources, &disk,
                       &child_live, &event_context))
            goto out;
        if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_STRESS) ||
            receive_event(&event_context, LUNA_ISOLATION_EVENT_DONE,
                          LUNA_ISOLATION_MODE_STRESS))
            goto out;
        unsigned long long destroy_begin = __builtin_ia32_rdtsc();
        if (release_child(&child, &resources, vka, &child_live)) goto out;
        printf("LUNA_LIFECYCLE_DESTROY_SAMPLE ns=%llu\n",
               manager_elapsed_ns(destroy_begin, tsc_frequency));
        if (round % 10 == 0)
            printf("luna: restart stress %d/%d\n", round,
                   LUNA_RESTART_STRESS_ROUNDS);
    }
    printf("LUNA_LIFECYCLE_BENCHMARK_OK rounds=%d\n",
           LUNA_RESTART_STRESS_ROUNDS);
    printf("LUNA_RESTART_STRESS_OK rounds=%d\n",
           LUNA_RESTART_STRESS_ROUNDS);
    printf("LUNA_PERSISTENCE_OK rounds=%d\n",
           LUNA_RESTART_STRESS_ROUNDS);
    printf("LUNA_NETWORK_RECLAIM_OK rounds=%d\n",
           LUNA_RESTART_STRESS_ROUNDS);

    if (boot_child(simple, vka, manager_vspace, control_ep.cptr,
                   command_ep.cptr, CHILD_BADGE_CLEAN,
                   LUNA_ISOLATION_MODE_CLEAN, private_addr, tsc_frequency,
                   &child, &resources, &disk, &child_live, &event_context))
        goto out;
    if (receive_event(&event_context, LUNA_ISOLATION_EVENT_NETWORK_ICMP_OK,
                      LUNA_ISOLATION_MODE_CLEAN) ||
        receive_event(&event_context, LUNA_ISOLATION_EVENT_NETWORK_TCP_OK,
                      LUNA_ISOLATION_MODE_CLEAN) ||
        receive_event(&event_context,
                      LUNA_ISOLATION_EVENT_NETWORK_PRESSURE_OK,
                      LUNA_ISOLATION_MODE_CLEAN) ||
        receive_event(&event_context,
                      LUNA_ISOLATION_EVENT_NETWORK_TX_PRESSURE_OK,
                      LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (luna_network_verify_irq()) {
        printf("luna: virtio-net IRQ verification failed\n");
        goto out;
    }
    if (receive_event(&event_context, LUNA_ISOLATION_EVENT_USER_PROGRAM_OK,
                      LUNA_ISOLATION_MODE_CLEAN) ||
        receive_event(&event_context, LUNA_ISOLATION_EVENT_LKL_SHELL_READY,
                      LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    printf("LUNA_NETWORK_ICMP_OK peer=10.0.2.2\n");
    printf("LUNA_NETWORK_TCP_OK peer=10.0.2.2:18080\n");
    printf("LUNA_NETWORK_PRESSURE_OK burst=%lu payload=%lu\n",
           (unsigned long)LUNA_NET_STRESS_BURST,
           (unsigned long)LUNA_NET_STRESS_PAYLOAD);
    printf("LUNA_NETWORK_TX_PRESSURE_OK packets=%lu payload=%lu\n",
           (unsigned long)LUNA_NET_TX_STRESS_PACKETS,
           (unsigned long)LUNA_NET_TX_STRESS_PAYLOAD);
    printf("LUNA_PHASE2_4_USER_OK\n");
    printf("LUNA_LKL_CHILD_SHELL_READY\n");
    if (wait_child_halt(&event_context, LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    if (receive_event(&event_context, LUNA_ISOLATION_EVENT_DONE,
                      LUNA_ISOLATION_MODE_CLEAN))
        goto out;
    printf("LUNA_ISOLATION_CHANNEL_OK\n");
    printf("LUNA_ISOLATION_RESTART_OK\n");
    if (release_child(&child, &resources, vka, &child_live)) goto out;
    result = 0;

out:
    if (child_live) (void)destroy_child(&child, &resources, vka);
    if (command_allocated) vka_free_object(vka, &command_ep);
    if (control_allocated) vka_free_object(vka, &control_ep);
    /* The persistent disk is manager-owned state and intentionally remains
     * allocated until this root task's terminal quiescent state. */
    vspace_unmap_pages(manager_vspace, MANAGER_PRIVATE_ADDR, 1,
                       seL4_PageBits, vka);
    vspace_free_reservation(manager_vspace, private_reservation);
    return result;
}
