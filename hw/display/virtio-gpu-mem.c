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
#include "hw/virtio/virtio-gpu-bswap.h"

#include "trace.h"

int virtio_gpu_create_iov(VirtIOGPU *g,
                          struct virtio_gpu_mem_entry *ents,
                          int nr_entries,
                          uint64_t **addr, struct iovec **iov,
                          uint64_t *size)
{
    int i;

    *iov = g_malloc0(sizeof(struct iovec) * nr_entries);
    if (addr) {
        *addr = g_malloc0(sizeof(uint64_t) * nr_entries);
    }
    if (size) {
        *size = 0;
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
        if (size) {
            *size += len;
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
virtio_gpu_memory_region_new(VirtIOGPU *g, uint32_t memory_id,
                             enum virtio_gpu_memory_type memory_type,
                             bool guest_ref)
{
    struct virtio_gpu_memory_region *mem;

    mem = g_new0(struct virtio_gpu_memory_region, 1);
    mem->memory_id = memory_id;
    mem->memory_type = memory_type;
    mem->guest_ref = guest_ref;
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

    virtio_gpu_cleanup_iov(g, mem->iov, mem->iov_cnt);
    g_free(mem->addrs);
    QTAILQ_REMOVE(&g->memlist, mem, next);
    g_free(mem);
}

struct virtio_gpu_memory_region*
virtio_gpu_memory_region_find(VirtIOGPU *g, uint32_t memory_id)
{
    struct virtio_gpu_memory_region *mem;

    QTAILQ_FOREACH(mem, &g->memlist, next) {
        if (!mem->guest_ref) {
            continue;
        }
        if (mem->memory_id == memory_id) {
            return mem;
        }
    }
    return NULL;
}

void virtio_gpu_memory_region_save(QEMUFile *f, VirtIOGPU *g,
                                   struct virtio_gpu_memory_region *mem)
{
    unsigned int i;

    for (i = 0; i < mem->iov_cnt; i++) {
        qemu_put_be64(f, mem->addrs[i]);
        qemu_put_be32(f, mem->iov[i].iov_len);
    }
}

int virtio_gpu_memory_region_load(QEMUFile *f, VirtIOGPU *g,
                                  struct virtio_gpu_memory_region *mem,
                                  unsigned int iov_cnt)
{
    unsigned int i;

    mem->iov_cnt = iov_cnt;
    mem->addrs = g_new(uint64_t, iov_cnt);
    mem->iov = g_new(struct iovec, iov_cnt);

    /* read data */
    for (i = 0; i < iov_cnt; i++) {
        mem->addrs[i] = qemu_get_be64(f);
        mem->iov[i].iov_len = qemu_get_be32(f);
    }

    /* restore mapping */
    for (i = 0; i < iov_cnt; i++) {
        hwaddr len = mem->iov[i].iov_len;
        mem->iov[i].iov_base =
            dma_memory_map(VIRTIO_DEVICE(g)->dma_as,
                           mem->addrs[i], &len, DMA_DIRECTION_TO_DEVICE);

        if (!mem->iov[i].iov_base || len != mem->iov[i].iov_len) {
            /* Clean up the half-a-mapping we just created... */
            if (mem->iov[i].iov_base) {
                dma_memory_unmap(VIRTIO_DEVICE(g)->dma_as,
                                 mem->iov[i].iov_base,
                                 mem->iov[i].iov_len,
                                 DMA_DIRECTION_TO_DEVICE,
                                 mem->iov[i].iov_len);
            }
            /* ...and the mappings for previous loop iterations */
            virtio_gpu_cleanup_iov(g, mem->iov, i);
            return -EINVAL;
        }
    }
    return 0;
}

bool virtio_gpu_check_memory_type(VirtIOGPU *g,
                                  enum virtio_gpu_memory_type memory_type)
{
    switch (memory_type) {
    case VIRTIO_GPU_MEMORY_TRANSFER:
        break;
    default:
        return false;
    }
    return true;
}

void virtio_gpu_cmd_memory_create(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_memory_create create;
    struct virtio_gpu_memory_region *mem;
    struct virtio_gpu_mem_entry *ents;
    size_t esize, s;
    int ret;

    VIRTIO_GPU_FILL_CMD(create);
    virtio_gpu_bswap_32(&create, sizeof(create));
    trace_virtio_gpu_cmd_mem_create(create.memory_id);

    if (create.memory_id == 0 || create.memory_id == -1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: memory region id is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_MEMORY_ID;
        return;
    }

    if (!virtio_gpu_check_memory_type(g, create.memory_type)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: memory type %d check failed\n",
                      __func__, create.memory_type);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    mem = virtio_gpu_memory_region_find(g, create.memory_id);
    if (mem) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: memory region already exists %d\n",
                      __func__, create.memory_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_MEMORY_ID;
        return;
    }

    if (create.nr_entries > 16384) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: nr_entries is too big (%d > 16384)\n",
                      __func__, create.nr_entries);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    esize = sizeof(*ents) * create.nr_entries;
    ents = g_malloc(esize);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(create), ents, esize);
    if (s != esize) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command data size incorrect %zu vs %zu\n",
                      __func__, s, esize);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        g_free(ents);
        return;
    }

    mem = virtio_gpu_memory_region_new(g, create.memory_id,
                                       create.memory_type, true);
    ret = virtio_gpu_create_iov(g, ents, create.nr_entries,
                                &mem->addrs, &mem->iov, &mem->size);
    g_free(ents);

    if (ret < 0) {
        virtio_gpu_memory_region_unref(g, mem);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    mem->iov_cnt = create.nr_entries;
    return;
}

void virtio_gpu_cmd_memory_unref(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_memory_unref unref;
    struct virtio_gpu_memory_region *mem;

    VIRTIO_GPU_FILL_CMD(unref);
    virtio_gpu_bswap_32(&unref, sizeof(unref));
    trace_virtio_gpu_cmd_mem_unref(unref.memory_id);

    if (unref.memory_id == 0 || unref.memory_id == -1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: memory region id is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_MEMORY_ID;
        return;
    }

    mem = virtio_gpu_memory_region_find(g, unref.memory_id);
    if (!mem) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: memory region not found %d\n",
                      __func__, unref.memory_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_MEMORY_ID;
        return;
    }

    mem->guest_ref = false;
    virtio_gpu_memory_region_unref(g, mem);
    return;
}
