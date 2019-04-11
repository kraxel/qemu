/*
 * Virtio GPU Device - memory regions
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "sysemu/dma.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"

int virtio_gpu_create_iov(VirtIOGPU *g,
                          struct virtio_gpu_mem_entry *ents,
                          int nr_entries,
                          uint64_t **addr, struct iovec **iov)
{
    int i;

    *iov = g_malloc0(sizeof(struct iovec) * nr_entries);
    if (addr) {
        *addr = g_malloc0(sizeof(uint64_t) * nr_entries);
    }
    for (i = 0; i < nr_entries; i++) {
        uint64_t a = le64_to_cpu(ents[i].addr);
        uint32_t l = le32_to_cpu(ents[i].length);
        hwaddr len = l;
        (*iov)[i].iov_len = l;
        (*iov)[i].iov_base = dma_memory_map(VIRTIO_DEVICE(g)->dma_as,
                                            a, &len, DMA_DIRECTION_TO_DEVICE);
        if (addr) {
            (*addr)[i] = a;
        }
        if (!(*iov)[i].iov_base || len != l) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: dma_memory_map failed\n",
                          __func__);
            virtio_gpu_cleanup_iov(g, *iov, i);
            g_free(ents);
            *iov = NULL;
            if (addr) {
                g_free(*addr);
                *addr = NULL;
            }
            return -1;
        }
    }
    return 0;
}

void virtio_gpu_cleanup_iov(VirtIOGPU *g,
                            struct iovec *iov, uint32_t count)
{
    int i;

    for (i = 0; i < count; i++) {
        dma_memory_unmap(VIRTIO_DEVICE(g)->dma_as,
                         iov[i].iov_base, iov[i].iov_len,
                         DMA_DIRECTION_TO_DEVICE,
                         iov[i].iov_len);
    }
    g_free(iov);
}

struct virtio_gpu_memory_region*
virtio_gpu_memory_region_new(VirtIOGPU *g, uint32_t memory_id)
{
    struct virtio_gpu_memory_region *mem;

    mem = g_new0(struct virtio_gpu_memory_region, 1);
    mem->memory_id = memory_id;
    atomic_inc(&mem->ref);
    QTAILQ_INSERT_HEAD(&g->memlist, mem, next);
    return mem;
}

struct virtio_gpu_memory_region*
virtio_gpu_memory_region_ref(VirtIOGPU *g,
                             struct virtio_gpu_memory_region *mem)
{
    if (!mem) {
        return NULL;
    }
    atomic_inc(&mem->ref);
    return mem;
}

void virtio_gpu_memory_region_unref(VirtIOGPU *g,
                                    struct virtio_gpu_memory_region *mem)
{
    if (!mem) {
        return;
    }
    if (atomic_dec_fetch(&mem->ref) > 0) {
        return;
    }

    g_free(mem->addrs);
    QTAILQ_REMOVE(&g->memlist, mem, next);
    g_free(mem);
}

struct virtio_gpu_memory_region*
virtio_gpu_memory_region_find(VirtIOGPU *g, uint32_t memory_id)
{
    struct virtio_gpu_memory_region *mem;

    QTAILQ_FOREACH(mem, &g->memlist, next) {
        if (mem->memory_id == memory_id) {
            return mem;
        }
    }
    return NULL;
}
