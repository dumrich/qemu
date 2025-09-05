#include "qemu/osdep.h"
#include "hw/isa/i8259_internal.h"
#include "hw/intc/i8259.h"
#include "qemu/module.h"
#include "system/bhyve.h"
#include "hw/irq.h"
#include "qom/object.h"
#include "bhyve-internal.h"

/**
 * BhyvePICClass:
 * @parent_realize: The parent's realizefn.
 */
typedef struct {
    PICCommonClass parent_class;

    DeviceRealize parent_realize;
} BhyvePICClass;

#define TYPE_BHYVE_I8259 "bhyve-i8259"
DECLARE_CLASS_CHECKERS(BhyvePICClass, BHYVE_PIC,
                       TYPE_BHYVE_I8259)


static void bhyve_pic_get(PICCommonState *s)
{
}

static void bhyve_pic_put(PICCommonState *s)
{
}

static void bhyve_pic_reset(DeviceState *dev)
{
    PICCommonState *s = PIC_COMMON(dev);

    s->elcr = 0;
    pic_reset_common(s);

    bhyve_pic_put(s);
}

static void bhyve_pic_set_irq(void *opaque, int irq, int level)
{
    int err;
    pic_stat_update_irq(irq, level);
    /* TODO: Add IOAPIC support */
    if (level) {
        err = vm_isa_assert_irq(bhyve_mach.vm, irq, -1);
    } else {
        err = vm_isa_deassert_irq(bhyve_mach.vm, irq, -1);
    }

    if (err) {
        fprintf(stderr, "bhyve: 8259 failed, irq (%d)",
                irq);
    }
}

static void bhyve_pic_realize(DeviceState *dev, Error **errp)
{
    PICCommonState *s = PIC_COMMON(dev);
    BhyvePICClass *bpc = BHYVE_PIC_GET_CLASS(dev);

    memory_region_init_io(&s->base_io, OBJECT(dev), NULL, NULL, "bhyve-pic", 2);
    memory_region_init_io(&s->elcr_io, OBJECT(dev), NULL, NULL, "bhyve-elcr", 1);

    bpc->parent_realize(dev, errp);
}

qemu_irq *bhyve_i8259_init(ISABus *bus)
{
    i8259_init_chip(TYPE_BHYVE_I8259, bus, true);
    i8259_init_chip(TYPE_BHYVE_I8259, bus, false);

    return qemu_allocate_irqs(bhyve_pic_set_irq, NULL, ISA_NUM_IRQS);
}

static void bhyve_i8259_class_init(ObjectClass *klass, const void *data)
{
    BhyvePICClass *bpc = BHYVE_PIC_CLASS(klass);
    PICCommonClass *k = PIC_COMMON_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bhyve_pic_reset);
    device_class_set_parent_realize(dc, bhyve_pic_realize, &bpc->parent_realize);
    k->pre_save   = bhyve_pic_get;
    k->post_load  = bhyve_pic_put;
}

static const TypeInfo bhyve_i8259_info = {
    .name = TYPE_BHYVE_I8259,
    .parent = TYPE_PIC_COMMON,
    .instance_size = sizeof(PICCommonState),
    .class_init = bhyve_i8259_class_init,
    .class_size = sizeof(BhyvePICClass),
};

static void bhyve_pic_register_types(void)
{
    type_register_static(&bhyve_i8259_info);
}

type_init(bhyve_pic_register_types)
