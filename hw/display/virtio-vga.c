#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "ui/console.h"
#include "vga_int.h"
#include "hw/virtio/virtio-pci.h"

/*
 * virtio-vga: This extends VirtioPCIProxy.
 */
#define TYPE_VIRTIO_VGA "virtio-vga"
#define VIRTIO_VGA(obj) \
        OBJECT_CHECK(VirtIOVGA, (obj), TYPE_VIRTIO_VGA)

typedef struct VirtIOVGA {
    VirtIOPCIProxy parent_obj;
    VirtIOGPU      vdev;
    VGACommonState vga;
} VirtIOVGA;

static void virtio_vga_invalidate_display(void *opaque)
{
    VirtIOVGA *vvga = opaque;

    if (vvga->vdev.enable) {
        virtio_gpu_ops.invalidate(&vvga->vdev);
    } else {
        vvga->vga.hw_ops->invalidate(&vvga->vga);
    }
}

static void virtio_vga_update_display(void *opaque)
{
    VirtIOVGA *vvga = opaque;

    if (vvga->vdev.enable) {
        virtio_gpu_ops.gfx_update(&vvga->vdev);
    } else {
        vvga->vga.hw_ops->gfx_update(&vvga->vga);
    }
}

static void virtio_vga_text_update(void *opaque, console_ch_t *chardata)
{
    VirtIOVGA *vvga = opaque;

    if (vvga->vdev.enable) {
        if (virtio_gpu_ops.text_update) {
            virtio_gpu_ops.text_update(&vvga->vdev, chardata);
        }
    } else {
        if (vvga->vga.hw_ops->text_update) {
            vvga->vga.hw_ops->text_update(&vvga->vga, chardata);
        }
    }
}

static int virtio_vga_ui_info(void *opaque, uint32_t idx, QemuUIInfo *info)
{
    VirtIOVGA *vvga = opaque;

    if (virtio_gpu_ops.ui_info) {
        return virtio_gpu_ops.ui_info(&vvga->vdev, idx, info);
    }
    return -1;
}

static const GraphicHwOps virtio_vga_ops = {
    .invalidate = virtio_vga_invalidate_display,
    .gfx_update = virtio_vga_update_display,
    .text_update = virtio_vga_text_update,
    .ui_info = virtio_vga_ui_info,
};

/* VGA device wrapper around PCI device around virtio GPU */
static int virtio_vga_init(VirtIOPCIProxy *vpci_dev)
{
    VirtIOVGA *vvga = VIRTIO_VGA(vpci_dev);
    VirtIOGPU *g = &vvga->vdev;
    VGACommonState *vga = &vvga->vga;

    qdev_set_parent_bus(DEVICE(g), BUS(&vpci_dev->bus));
    /* force virtio-1.0 */
    vpci_dev->flags &= ~VIRTIO_PCI_FLAG_DISABLE_MODERN;
    vpci_dev->flags |= VIRTIO_PCI_FLAG_DISABLE_LEGACY;
    /* avoid conflicts with vga framebuffer @ bar 2 */
    vpci_dev->modern_mem_bar = 0;
    vpci_dev->msix_bar = 4;
    if (qdev_init(DEVICE(g)) < 0) {
        return -1;
    }

    vga->vram_size_mb = 8;
    vga_common_init(vga, OBJECT(vpci_dev), false);
    vga_init(vga, OBJECT(vpci_dev), pci_address_space(&vpci_dev->pci_dev),
             pci_address_space_io(&vpci_dev->pci_dev), true);

    vga->con = g->scanout[0].con;
    graphic_console_set_hwops(vga->con, &virtio_vga_ops, vvga);

    pci_register_bar(&vpci_dev->pci_dev, 2,
                     PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);

    return 0;
}

static void virtio_vga_reset(DeviceState *dev)
{
    VirtIOVGA *vvga = VIRTIO_VGA(dev);
    vvga->vdev.enable = 0;

    vga_dirty_log_start(&vvga->vga);
}

static Property virtio_vga_properties[] = {
    DEFINE_VIRTIO_GPU_PROPERTIES(VirtIOVGA, vdev.conf),
    DEFINE_VIRTIO_GPU_PCI_PROPERTIES(VirtIOPCIProxy),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_vga_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->props = virtio_vga_properties;
    dc->reset = virtio_vga_reset;
    dc->hotpluggable = false;

    k->init = virtio_vga_init;
    pcidev_k->romfile = "vgabios-virtio.bin";
    pcidev_k->class_id = PCI_CLASS_DISPLAY_VGA;
}

static void virtio_vga_inst_initfn(Object *obj)
{
    VirtIOVGA *dev = VIRTIO_VGA(obj);
    object_initialize(&dev->vdev, sizeof(dev->vdev), TYPE_VIRTIO_GPU);
    object_property_add_child(obj, "virtio-backend", OBJECT(&dev->vdev), NULL);
}

static TypeInfo virtio_vga_info = {
    .name          = TYPE_VIRTIO_VGA,
    .parent        = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(struct VirtIOVGA),
    .instance_init = virtio_vga_inst_initfn,
    .class_init    = virtio_vga_class_init,
};

static void virtio_vga_register_types(void)
{
    type_register_static(&virtio_vga_info);
}

type_init(virtio_vga_register_types)
