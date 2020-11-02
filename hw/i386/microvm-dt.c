#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "sysemu/device_tree.h"
#include "hw/char/serial.h"
#include "hw/i386/fw_cfg.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/sysbus.h"
#include "hw/virtio/virtio-mmio.h"
#include "hw/usb/xhci.h"

#include "microvm-dt.h"

static bool debug = true;

static void dt_add_microvm_irq(MicrovmMachineState *mms, const char *nodename, uint32_t irq)
{
    int index = 0;

    if (irq >= IO_APIC_SECONDARY_IRQBASE) {
        irq -= IO_APIC_SECONDARY_IRQBASE;
        index++;
    }

    qemu_fdt_setprop_cell(mms->fdt, nodename, "interrupt-parent",
                          mms->ioapic_phandle[index]);
    qemu_fdt_setprop_cells(mms->fdt, nodename, "interrupts", irq, 0);
}

static void dt_add_virtio(MicrovmMachineState *mms, VirtIOMMIOProxy *mmio)
{
    SysBusDevice *dev = SYS_BUS_DEVICE(mmio);
    VirtioBusState *mmio_virtio_bus = &mmio->bus;
    BusState *mmio_bus = &mmio_virtio_bus->parent_obj;
    char *nodename;

    if (QTAILQ_EMPTY(&mmio_bus->children)) {
        return;
    }

    hwaddr base = dev->mmio[0].addr;
    hwaddr size = 512;
    unsigned index = (base - VIRTIO_MMIO_BASE) / size;
    uint32_t irq = mms->virtio_irq_base + index;

    nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop_string(mms->fdt, nodename, "compatible", "virtio,mmio");
    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg", 2, base, 2, size);
    qemu_fdt_setprop(mms->fdt, nodename, "dma-coherent", NULL, 0);
    dt_add_microvm_irq(mms, nodename, irq);
    g_free(nodename);
}

static void dt_add_xhci(MicrovmMachineState *mms)
{
    const char compat[] = "generic-xhci";
    uint32_t irq = MICROVM_XHCI_IRQ;
    hwaddr base = MICROVM_XHCI_BASE;
    hwaddr size = XHCI_LEN_REGS;
    char *nodename;

    nodename = g_strdup_printf("/usb@%" PRIx64, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop(mms->fdt, nodename, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg", 2, base, 2, size);
    qemu_fdt_setprop(mms->fdt, nodename, "dma-coherent", NULL, 0);
    dt_add_microvm_irq(mms, nodename, irq);
    g_free(nodename);
}

static void dt_add_pcie(MicrovmMachineState *mms)
{
    hwaddr base = PCIE_MMIO_BASE;
    int nr_pcie_buses;
    char *nodename;

    nodename = g_strdup_printf("/pcie@%" PRIx64, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop_string(mms->fdt, nodename,
                            "compatible", "pci-host-ecam-generic");
    qemu_fdt_setprop_string(mms->fdt, nodename, "device_type", "pci");
    qemu_fdt_setprop_cell(mms->fdt, nodename, "#address-cells", 3);
    qemu_fdt_setprop_cell(mms->fdt, nodename, "#size-cells", 2);
    qemu_fdt_setprop_cell(mms->fdt, nodename, "linux,pci-domain", 0);
    qemu_fdt_setprop(mms->fdt, nodename, "dma-coherent", NULL, 0);

    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg",
                                 2, PCIE_ECAM_BASE, 2, PCIE_ECAM_SIZE);
    if (0 /* mms->gpex.mmio64.size */) {
        qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "ranges",

#if 0
                                     1, FDT_PCI_RANGE_IOPORT,
                                     2, 0,
                                     2, 0,
                                     2, 0x10000,
#endif

                                     1, FDT_PCI_RANGE_MMIO,
                                     2, mms->gpex.mmio32.base,
                                     2, mms->gpex.mmio32.base,
                                     2, mms->gpex.mmio32.size,

                                     1, FDT_PCI_RANGE_MMIO_64BIT,
                                     2, mms->gpex.mmio64.base,
                                     2, mms->gpex.mmio64.base,
                                     2, mms->gpex.mmio64.size);
    } else {
        qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "ranges",

#if 0
                                     1, FDT_PCI_RANGE_IOPORT,
                                     2, 0,
                                     2, 0,
                                     2, 0x10000,
#endif

                                     1, FDT_PCI_RANGE_MMIO,
                                     2, mms->gpex.mmio32.base,
                                     2, mms->gpex.mmio32.base,
                                     2, mms->gpex.mmio32.size);
    }

    nr_pcie_buses = PCIE_ECAM_SIZE / PCIE_MMCFG_SIZE_MIN;
    qemu_fdt_setprop_cells(mms->fdt, nodename, "bus-range", 0,
                           nr_pcie_buses - 1);

    if (0 /* mms->msi_phandle */) {
        qemu_fdt_setprop_cells(mms->fdt, nodename, "msi-parent",
                               0 /* vms->msi_phandle */);
    }

    /* TODO: irqmap */
}

static void dt_add_ioapic(MicrovmMachineState *mms, SysBusDevice *dev)
{
    hwaddr base = dev->mmio[0].addr;
    char *nodename;
    uint32_t ph;
    int index;

    switch (base) {
    case IO_APIC_DEFAULT_ADDRESS:
        index = 0;
        break;
    case IO_APIC_SECONDARY_ADDRESS:
        index = 1;
        break;
    default:
        fprintf(stderr, "unknown ioapic @ %" PRIx64 "\n", base);
        return;
    }

    nodename = g_strdup_printf("/ioapic%d@%" PRIx64, index + 1, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop_string(mms->fdt, nodename,
                            "compatible", "intel,ce4100-ioapic");
    qemu_fdt_setprop(mms->fdt, nodename, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(mms->fdt, nodename, "#interrupt-cells", 0x2);
    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg",
                                 2, base, 2, 0x1000);

    ph = qemu_fdt_alloc_phandle(mms->fdt);
    qemu_fdt_setprop_cell(mms->fdt, nodename, "phandle", ph);
    qemu_fdt_setprop_cell(mms->fdt, nodename, "linux,phandle", ph);
    mms->ioapic_phandle[index] = ph;

    g_free(nodename);
}

static void dt_add_isa_serial(MicrovmMachineState *mms, ISADevice *dev)
{
    const char compat[] = "ns16550";
    uint32_t irq = object_property_get_int(OBJECT(dev), "irq", NULL);
    hwaddr base = object_property_get_int(OBJECT(dev), "iobase", NULL);
    hwaddr size = 8;
    char *nodename;

    nodename = g_strdup_printf("/serial@%" PRIx64, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop(mms->fdt, nodename, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg", 2, base, 2, size);
    dt_add_microvm_irq(mms, nodename, irq);

    if (base == 0x3f8 /* com1 */) {
        qemu_fdt_setprop_string(mms->fdt, "/chosen", "stdout-path", nodename);
    }

    g_free(nodename);
}

static void dt_add_isa_rtc(MicrovmMachineState *mms, ISADevice *dev)
{
    const char compat[] = "motorola,mc146818";
    uint32_t irq = RTC_ISA_IRQ;
    hwaddr base = RTC_ISA_BASE;
    hwaddr size = 8;
    char *nodename;

    nodename = g_strdup_printf("/rtc@%" PRIx64, base);
    qemu_fdt_add_subnode(mms->fdt, nodename);
    qemu_fdt_setprop(mms->fdt, nodename, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(mms->fdt, nodename, "reg", 2, base, 2, size);
    dt_add_microvm_irq(mms, nodename, irq);
    g_free(nodename);
}

static void dt_setup_isa_bus(MicrovmMachineState *mms, DeviceState *bridge)
{
    BusState *bus = qdev_get_child_bus(bridge, "isa.0");
    BusChild *kid;
    Object *obj;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;

        /* serial */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_ISA_SERIAL);
        if (obj) {
            dt_add_isa_serial(mms, ISA_DEVICE(obj));
            continue;
        }

        /* rtc */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_MC146818_RTC);
        if (obj) {
            dt_add_isa_rtc(mms, ISA_DEVICE(obj));
            continue;
        }

        if (debug) {
            fprintf(stderr, "%s: unhandled: %s\n", __func__,
                    object_get_typename(OBJECT(dev)));
        }
    }
}

static void dt_setup_sys_bus(MicrovmMachineState *mms)
{
    BusState *bus;
    BusChild *kid;
    Object *obj;

    /* sysbus devices */
    bus = sysbus_get_default();
    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;

        /* ioapic */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_IOAPIC);
        if (obj) {
            dt_add_ioapic(mms, SYS_BUS_DEVICE(obj));
            continue;
        }
    }

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        DeviceState *dev = kid->child;

        /* virtio */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_VIRTIO_MMIO);
        if (obj) {
            dt_add_virtio(mms, VIRTIO_MMIO(obj));
            continue;
        }

        /* xhci */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_XHCI_SYSBUS);
        if (obj) {
            dt_add_xhci(mms);
            continue;
        }

        /* pcie */
        obj = object_dynamic_cast(OBJECT(dev), TYPE_GPEX_HOST);
        if (obj) {
            dt_add_pcie(mms);
            continue;
        }

        /* isa */
        obj = object_dynamic_cast(OBJECT(dev), "isabus-bridge");
        if (obj) {
            dt_setup_isa_bus(mms, DEVICE(obj));
            continue;
        }

        if (debug) {
            obj = object_dynamic_cast(OBJECT(dev), TYPE_IOAPIC);
            if (obj) {
                /* ioapic already added in first pass */
                continue;
            }
            fprintf(stderr, "%s: unhandled: %s\n", __func__,
                    object_get_typename(OBJECT(dev)));
        }
    }
}

void dt_setup_microvm(MicrovmMachineState *mms)
{
    X86MachineState *x86ms = X86_MACHINE(mms);
    int size = 0;

    mms->fdt = create_device_tree(&size);

    /* root node */
    qemu_fdt_setprop_string(mms->fdt, "/", "compatible", "linux,microvm");
    qemu_fdt_setprop_cell(mms->fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(mms->fdt, "/", "#size-cells", 0x2);

    qemu_fdt_add_subnode(mms->fdt, "/chosen");
    dt_setup_sys_bus(mms);

    /* add to fw_cfg */
    fprintf(stderr, "%s: add etc/fdt to fw_cfg\n", __func__);
    fw_cfg_add_file(x86ms->fw_cfg, "etc/fdt", mms->fdt, size);

    if (debug) {
        fprintf(stderr, "%s: writing microvm.fdt\n", __func__);
        g_file_set_contents("microvm.fdt", mms->fdt, size, NULL);
        int ret = system("dtc -I dtb -O dts microvm.fdt");
        if (ret != 0) {
            fprintf(stderr, "%s: oops, dtc not installed?\n", __func__);
        }
    }
}
