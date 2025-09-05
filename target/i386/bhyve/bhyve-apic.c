#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "hw/i386/apic_internal.h"
#include "hw/i386/apic-msidef.h"
#include "hw/pci/msi.h"
#include "system/hw_accel.h"
#include "system/bhyve.h"
#include "bhyve-internal.h"
#include <vmmapi.h>

// Local APIC implementation with Bhyve

static void bhyve_put_apic_state(APICCommonState *s,
                                struct vm_lapic_state *kapic)
{
    int i;

    memset(kapic, 0, sizeof(*kapic));
    kapic->fields[0x2].data = s->id << 24;
    kapic->fields[0x3].data = s->version | ((APIC_LVT_NB - 1) << 16);
    kapic->fields[0x8].data = s->tpr;
    kapic->fields[0xd].data = s->log_dest << 24;
    kapic->fields[0xe].data = s->dest_mode << 28 | 0x0fffffff;
    kapic->fields[0xf].data = s->spurious_vec;
    for (i = 0; i < 8; i++) {
        kapic->fields[0x10 + i].data = s->isr[i];
        kapic->fields[0x18 + i].data = s->tmr[i];
        kapic->fields[0x20 + i].data = s->irr[i];
    }

    kapic->fields[0x28].data = s->esr;
    kapic->fields[0x30].data = s->icr[0];
    kapic->fields[0x31].data = s->icr[1];
    for (i = 0; i < APIC_LVT_NB; i++) {
        kapic->fields[0x32 + i].data = s->lvt[i];
    }

    kapic->fields[0x38].data = s->initial_count;
    kapic->fields[0x3e].data = s->divide_conf;
}

static void bhyve_get_apic_state(APICCommonState *s,
                                struct vm_lapic_state *kapic)
{
    int i, v;

    s->id = kapic->fields[0x2].data >> 24;
    s->tpr = kapic->fields[0x8].data;
    s->arb_id = kapic->fields[0x9].data;
    s->log_dest = kapic->fields[0xd].data >> 24;
    s->dest_mode = kapic->fields[0xe].data >> 28;
    s->spurious_vec = kapic->fields[0xf].data;
    for (i = 0; i < 8; i++) {
        s->isr[i] = kapic->fields[0x10 + i].data;
        s->tmr[i] = kapic->fields[0x18 + i].data;
        s->irr[i] = kapic->fields[0x20 + i].data;
    }

    s->esr = kapic->fields[0x28].data;
    s->icr[0] = kapic->fields[0x30].data;
    s->icr[1] = kapic->fields[0x31].data;
    for (i = 0; i < APIC_LVT_NB; i++) {
        s->lvt[i] = kapic->fields[0x32 + i].data;
    }

    s->initial_count = kapic->fields[0x38].data;
    s->divide_conf = kapic->fields[0x3e].data;

    v = (s->divide_conf & 3) | ((s->divide_conf >> 1) & 4);
    s->count_shift = (v + 1) & 7;

    s->initial_count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    apic_next_timer(s, s->initial_count_load_time);
}

static int bhyve_apic_set_base(APICCommonState *s, uint64_t val)
{
    s->apicbase = val;
    return 0;
}

static void bhyve_put_apic_base(CPUState *cpu, uint64_t val)
{
    /* Doesn't do anything */
}

static void bhyve_apic_set_tpr(APICCommonState *s, uint8_t val)
{
    s->tpr = val;
}

static uint8_t bhyve_apic_get_tpr(APICCommonState *s)
{
    return s->tpr;
}

static void bhyve_apic_vapic_base_update(APICCommonState *s)
{
    /* not implemented yet */
}

static void bhyve_apic_put(CPUState *cs, run_on_cpu_data data)
{
    APICCommonState *s = data.host_ptr;
    struct vcpu *vcpu = bhyve_get_vcpu(cs);
    struct vm_lapic_state kapic;
    int err;

    bhyve_put_apic_base(CPU(s->cpu), s->apicbase); // Doesn't do anything
    bhyve_put_apic_state(s, &kapic);

    err = vm_lapic_set_state(vcpu, &kapic);
    if (err) {
        fprintf(stderr,
            "VMM set LAPIC state failed: %08lx\n",
             err);
    }
}

static void bhyve_apic_get(DeviceState *dev)
{
    APICCommonState *s = APIC_COMMON(dev);
    CPUState *cpu = CPU(s->cpu);
    struct vcpu *vcpu = bhyve_get_vcpu(cpu);
    struct vm_lapic_state kapic;

    int err = vm_lapic_get_state(vcpu, &kapic);
    if (err) {
        fprintf(stderr,
            "VMM get LAPIC state failed: %08lx\n",
            err);
    }

    bhyve_get_apic_state(s, &kapic);
}

static void bhyve_apic_post_load(APICCommonState *s)
{
    run_on_cpu(CPU(s->cpu), bhyve_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void bhyve_apic_external_nmi(APICCommonState *s)
{
}

static void bhyve_send_msi(MSIMessage *msg)
{
    uint64_t addr = msg->address;
    uint32_t data = msg->data;

    int err = vm_lapic_msi(bhyve_mach.vm, addr + 0xFEE00000, data);
    if (err) {
        fprintf(stderr, "bhyve: injection failed, MSI (%llx, %x)",
                addr, data);
    }
}

static uint64_t bhyve_apic_mem_read(void *opaque, hwaddr addr,
                                   unsigned size)
{
    return ~(uint64_t)0;
}

static void bhyve_apic_mem_write(void *opaque, hwaddr addr,
                                uint64_t data, unsigned size)
{
    MSIMessage msg = { .address = addr, .data = data };
    bhyve_send_msi(&msg);
}

static const MemoryRegionOps bhyve_apic_io_ops = {
    .read = bhyve_apic_mem_read,
    .write = bhyve_apic_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void bhyve_apic_reset(APICCommonState *s)
{
    s->wait_for_sipi = 0;

    run_on_cpu(CPU(s->cpu), bhyve_apic_put, RUN_ON_CPU_HOST_PTR(s));
}

static void bhyve_apic_realize(DeviceState *dev, Error **errp)
{
    APICCommonState *s = APIC_COMMON(dev);

    memory_region_init_io(&s->io_memory, OBJECT(s), &bhyve_apic_io_ops, s,
                          "bhyve-apic-msi", APIC_SPACE_SIZE);

    msi_nonbroken = true;
}

static void bhyve_apic_class_init(ObjectClass *klass, const void *data)
{
    APICCommonClass *k = APIC_COMMON_CLASS(klass);

    k->realize = bhyve_apic_realize;
    k->reset = bhyve_apic_reset;
    k->set_base = bhyve_apic_set_base;
    k->set_tpr = bhyve_apic_set_tpr;
    k->get_tpr = bhyve_apic_get_tpr;
    k->post_load = bhyve_apic_post_load;
    k->vapic_base_update = bhyve_apic_vapic_base_update;
    k->external_nmi = bhyve_apic_external_nmi;
    k->send_msi = bhyve_send_msi;
}

static const TypeInfo bhyve_apic_info = {
    .name = "bhyve-apic",
    .parent = TYPE_APIC_COMMON,
    .instance_size = sizeof(APICCommonState),
    .class_init = bhyve_apic_class_init,
};

static void bhyve_apic_register_types(void)
{
    type_register_static(&bhyve_apic_info);
}

type_init(bhyve_apic_register_types)
