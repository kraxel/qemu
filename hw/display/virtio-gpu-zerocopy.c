/*
 * Virtio GPU Device
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
#include "qemu/units.h"
#include "qemu-common.h"
#include "qemu/iov.h"
#include "ui/console.h"
#include "hw/virtio/virtio-gpu.h"
#include "trace.h"

#ifdef CONFIG_LINUX

#include "cpu.h"
#include "exec/address-spaces.h"
#include "exec/ram_addr.h"

#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include "standard-headers/linux/udmabuf.h"

static QemuDmaBuf *virtio_gpu_create_udmabuf(struct virtio_gpu_simple_resource *res)
{
    struct udmabuf_create_list *list;
    QemuDmaBuf *dmabuf;
    MemoryRegion *mr;
    FlatView *fv;
    int memfd, udmabuf, i;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0) {
        return NULL;
    }

    list = g_malloc0(sizeof(struct udmabuf_create_list) +
                     sizeof(struct udmabuf_create_item) * res->iov_cnt);

    for (i = 0; i < res->iov_cnt; i++) {
        uint64_t a = res->addrs[i];
        uint32_t l = res->iov[i].iov_len;
        hwaddr xlat, len = l;

        rcu_read_lock();
        fv = address_space_to_flatview(&address_space_memory);
        mr = flatview_translate(fv, a, &xlat, &len, true,
                                MEMTXATTRS_UNSPECIFIED);
        memfd = mr->ram_block->fd;
        rcu_read_unlock();

        list->list[i].memfd  = memfd;
        list->list[i].offset = xlat;
        list->list[i].size   = len;
    }
    list->count = res->iov_cnt;
    list->flags = UDMABUF_FLAGS_CLOEXEC;

    dmabuf = g_new0(QemuDmaBuf, 1);
    dmabuf->fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);
    if (dmabuf->fd < 0) {
        warn_report("%s: UDMABUF_CREATE_LIST: %s", __func__,
                    strerror(errno));
        g_free(dmabuf);
        dmabuf = NULL;
    } else {
        uint32_t pformat = virtio_gpu_get_pixman_format(res->format);
        dmabuf->width  = res->width;
        dmabuf->height = res->height;
        dmabuf->stride = res->stride;
        dmabuf->fourcc = qemu_pixman_to_drm_format(pformat);
    }

    g_free(list);
    return dmabuf;
}

static void virtio_gpu_remap_udmabuf(struct virtio_gpu_simple_resource *res)
{
    res->mapsz = QEMU_ALIGN_UP(res->dmabuf->stride *
                               res->dmabuf->height,
                               getpagesize());
    res->mapptr = mmap(NULL, res->mapsz, PROT_READ,
                       MAP_SHARED, res->dmabuf->fd, 0);
    if (res->mapptr == MAP_FAILED) {
        warn_report("%s: dmabuf mmap failed: %s", __func__,
                    strerror(errno));
        res->mapptr = NULL;
    }
}

static void virtio_gpu_destroy_udmabuf(struct virtio_gpu_simple_resource *res)
{
    if (res->mapptr) {
        munmap(res->mapptr, res->mapsz);
        res->mapptr = NULL;
    }
    close(res->dmabuf->fd);
    free(res->dmabuf);
    res->dmabuf = NULL;
}

bool virtio_gpu_have_zerocopy(void)
{
    int udmabuf;

    udmabuf = udmabuf_fd();
    if (udmabuf < 0)
        return false;

    /*
     * FIXME: should check ram is memfd-backed?
     */
    return true;
}

void virtio_gpu_init_zerocopy(struct virtio_gpu_simple_resource *res)
{
    uint32_t pformat = virtio_gpu_get_pixman_format(res->format);
    void *pdata;

    fprintf(stderr, "%s: id %d, %dx%d, %d iov(s), %s\n", __func__,
            res->resource_id, res->width, res->height, res->iov_cnt,
            res->iov_cnt == 1 ? "direct" : "dmabuf");

    if (res->iov_cnt == 1) {
        pdata = res->iov[0].iov_base;
    } else {
        res->dmabuf = virtio_gpu_create_udmabuf(res);
        if (!res->dmabuf) {
            return;
        }
        virtio_gpu_remap_udmabuf(res);
        if (!res->mapptr) {
            return;
        }
        pdata = res->mapptr;
    }

    pixman_image_unref(res->image);
    res->image = pixman_image_create_bits(pformat,
                                          res->width, res->height,
                                          pdata, res->stride);
}

void virtio_gpu_fini_zerocopy(struct virtio_gpu_simple_resource *res)
{
    if (res->dmabuf) {
        fprintf(stderr, "%s: id %d, destroy dmabuf\n", __func__, res->resource_id);
        virtio_gpu_destroy_udmabuf(res);
    }
}

#else

bool virtio_gpu_have_zerocopy(void)
{
    /* nothing (stub) */
    return false;
}

void virtio_gpu_init_zerocopy(struct virtio_gpu_simple_resource *res)
{
    /* nothing (stub) */
}

void virtio_gpu_fini_zerocopy(struct virtio_gpu_simple_resource *res)
{
    /* nothing (stub) */
}

#endif
