
#include "qemu/osdep.h"
#include "monitor/monitor.h"
#include "hw/qdev-properties.h"
#include "hw/intc/ioapic_internal.h"
#include "system/bhyve.h"
#include "bhyve-internal.h"

typedef struct BhyveIOAPICState BhyveIOAPICState;

struct BhyveIOAPICState {
    IOAPICCommonState ioapic;
};

static void bhyve_ioapic_get(IOAPICCommonState *s)
{
}

static void bhyve_ioapic_put(IOAPICCommonState *s)
{
}

static void bhyve_ioapic_reset(DeviceState *dev)
{
    IOAPICCommonState *s = IOAPIC_COMMON(dev);

    ioapic_reset_common(dev);
    bhyve_ioapic_put(s);
}

static void bhyve_ioapic_set_irq(void *opaque, int irq, int level)
{
    BhyveIOAPICState *s = opaque;
    IOAPICCommonState *common = IOAPIC_COMMON(s);
    int err;

    ioapic_stat_update_irq(common, irq, level);

    if (level) {
        err = vm_ioapic_assert_irq(bhyve_mach.vm, (irq == 0) ? 2 : irq);
    } else {
        err = vm_ioapic_deassert_irq(bhyve_mach.vm, (irq == 0) ? 2 : irq);
    }

    if (err) {
        fprintf(stderr, "Could not assert IRQ %d\n", irq);
    }
}

static void bhyve_ioapic_realize(DeviceState *dev, Error **errp)
{
    IOAPICCommonState *s = IOAPIC_COMMON(dev);

    memory_region_init_io(&s->io_memory, OBJECT(dev), NULL, NULL, "bhyve-ioapic", 0x1000);
    /* TODO: Fix */
    s->version = 0x11;

    qdev_init_gpio_in(dev, bhyve_ioapic_set_irq, IOAPIC_NUM_PINS);
}

static void bhyve_ioapic_class_init(ObjectClass *klass, const void *data)
{
    IOAPICCommonClass *k = IOAPIC_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize   = bhyve_ioapic_realize;
    k->pre_save  = bhyve_ioapic_get;
    k->post_load = bhyve_ioapic_put;
    device_class_set_legacy_reset(dc, bhyve_ioapic_reset);
}

static const TypeInfo bhyve_ioapic_info = {
    .name  = TYPE_BHYVE_IOAPIC,
    .parent = TYPE_IOAPIC_COMMON,
    .instance_size = sizeof(BhyveIOAPICState),
    .class_init = bhyve_ioapic_class_init,
};

static void bhyve_ioapic_register_types(void)
{
    type_register_static(&bhyve_ioapic_info);
}

type_init(bhyve_ioapic_register_types)
