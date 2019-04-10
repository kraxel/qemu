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
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"

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
