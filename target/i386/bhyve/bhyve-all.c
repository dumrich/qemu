#include "qemu/osdep.h"
#include "qemu/accel.h"
#include "accel/accel-ops.h"
#include "hw/boards.h"
#include "qemu/typedefs.h"
#include "system/runstate.h"
#include "system/bhyve.h"
#include "cpu.h"
#include "system/address-spaces.h"
#include "system/ioport.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "strings.h"
#include "qapi/qapi-types-common.h"
#include "qapi/qapi-visit-common.h"

#include "bhyve-accel-ops.h"
#include "bhyve-internal.h"

#include <signal.h>
#include <unistd.h>

#include <machine/specialreg.h>
#include <string.h>
#include <vmmapi.h>

/* -------------------------------------------------------------------------- */

#define VM_NAME "vm0"

struct AccelCPUState {
    struct vcpu *vcpu;
    uint64_t tpr;

    /* QEMU State =/= VMM state */
    bool dirty;
};

/* -------------------------------------------------------------------------- */

static bool bhyve_allowed;

int
bhyve_enabled(void) {
    return bhyve_allowed;
}

static enum host_vendor get_cpu_vendor(void) {
    unsigned int eax = 0;
    unsigned int regs[4];
    char vendor[13];

    __asm__ __volatile__("cpuid"
                         : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]),
                           "=d"(regs[3])
                         : "a"(eax)
    );

    *(unsigned int *)(vendor + 0) = regs[0]; /* EBX */
    *(unsigned int *)(vendor + 4) = regs[2]; /* EDX */
    *(unsigned int *)(vendor + 8) = regs[1]; /* ECX */
    vendor[12] = '\0';

    if (strstr(vendor, "Intel")) {
        return VENDOR_INTEL;
    } else if (strstr(vendor, "AMD")) {
        return VENDOR_AMD;
    } else {
        return VENDOR_UNKNOWN;
    }
}

/* -------------------------------------------------------------------------- */

struct bhyve_machine bhyve_mach;

int segid_num = 1;

/* -------------------------------------------------------------------------- */

struct vcpu *bhyve_get_vcpu(CPUState *cpu) {
    return cpu->accel->vcpu;
}

static struct bhyve_machine *get_bhyve_mach(void) {
    return &bhyve_mach;
}

static void dump_registers(struct vcpu *vcpu) {
    uint64_t val;
    int err;

    printf("\n=== Register Dump ===\n");

    const struct {
        const char *name;
        int reg;
    } regs[] = {
        { "RAX", VM_REG_GUEST_RAX },
        { "RBX", VM_REG_GUEST_RBX },
        { "RCX", VM_REG_GUEST_RCX },
        { "RDX", VM_REG_GUEST_RDX },
        { "RSI", VM_REG_GUEST_RSI },
        { "RDI", VM_REG_GUEST_RDI },
        { "RBP", VM_REG_GUEST_RBP },
        { "RSP", VM_REG_GUEST_RSP },
        { "RIP", VM_REG_GUEST_RIP },
        { "RFLAGS", VM_REG_GUEST_RFLAGS },
    };

    for (int i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        err = vm_get_register(vcpu, regs[i].reg, &val);
        if (err != 0) {
            printf("%s: ERROR (%d)\n", regs[i].name, err);
        } else {
            printf("%-8s = 0x%016" PRIx64 "\n", regs[i].name, val);
        }
    }

    const struct {
        const char *name;
        int reg;
    } segs[] = {
        { "CS", VM_REG_GUEST_CS },
        { "DS", VM_REG_GUEST_DS },
        { "ES", VM_REG_GUEST_ES },
        { "SS", VM_REG_GUEST_SS },
        { "FS", VM_REG_GUEST_FS },
        { "GS", VM_REG_GUEST_GS },
    };

    struct seg_desc desc;
    for (int i = 0; i < sizeof(segs)/sizeof(segs[0]); i++) {
        err = vm_get_seg_desc(vcpu, segs[i].reg, &desc);
        if (err != 0) {
            printf("%s: ERROR (%d)\n", segs[i].name, err);
        } else {
            printf("%s: base=0x%016" PRIx64 " limit=0x%08x access=0x%08x\n",
                segs[i].name, desc.base, desc.limit, desc.access);
        }
    }

    printf("======================\n\n");
}

/* -------------------------------------------------------------------------- */

/*
static int
vmm_set_segment(struct vcpu *vcpu, int reg, const SegmentCache *qseg)
{
    int error;
    error = vm_set_register(vcpu, reg, qseg->selector);
    if (error) {
        return -1;
    }

    error = vm_set_desc(vcpu, reg, qseg->base, qseg->limit, qseg->flags);
    if (error) {
        return -1;
    }
    return 0;
}

static int
vmm_get_segment(struct vcpu *vcpu, int reg, const SegmentCache *qseg)
{
    int error;
    error = vm_get_register(vcpu, reg, (const uint64_t*)&qseg->flags);
    if (error) {
        return error;
    }
    error = vm_get_desc(vcpu, reg, (const unsigned long*)&qseg->base, &qseg->limit, (const unsigned int*)&qseg->flags);
    if (error) {
        return error;
    }
    return 0;
}
*/

static int vmm_set_registers(CPUState *cpu) {
    CPUX86State *env = cpu_env(cpu);
    AccelCPUState *qcpu = cpu->accel;
    struct vcpu* vcpu = qcpu->vcpu;

    /* GPRs */
    int ret;

    // General-Purpose Registers
    ret = vm_set_register(vcpu, VM_REG_GUEST_RAX, env->regs[R_EAX]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RBX, env->regs[R_EBX]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RCX, env->regs[R_ECX]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RDX, env->regs[R_EDX]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RSI, env->regs[R_ESI]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RDI, env->regs[R_EDI]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RBP, env->regs[R_EBP]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RSP, env->regs[R_ESP]);
    if (ret != 0) return ret;
#ifdef TARGET_X86_64
    ret = vm_set_register(vcpu, VM_REG_GUEST_R8, env->regs[R_R8]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R9, env->regs[R_R9]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R10, env->regs[R_R10]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R11, env->regs[R_R11]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R12, env->regs[R_R12]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R13, env->regs[R_R13]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R14, env->regs[R_R14]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_R15, env->regs[R_R15]);
    if (ret != 0) return ret;
#endif

    // RIP and RFLAGS
    ret = vm_set_register(vcpu, VM_REG_GUEST_RIP, env->eip);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_RFLAGS, env->eflags);
    if (ret != 0) return ret;

    // Segment Registers (Selectors only)
    ret = vm_set_register(vcpu, VM_REG_GUEST_CS, env->segs[R_CS].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DS, env->segs[R_DS].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_ES, env->segs[R_ES].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_SS, env->segs[R_SS].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_FS, env->segs[R_FS].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_GS, env->segs[R_GS].selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_LDTR, env->ldt.selector);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_TR, env->tr.selector);
    if (ret != 0) return ret;

    // Descriptor Table Registers (Base and Limit)
    ret = vm_set_register(vcpu, VM_REG_GUEST_GDTR, env->gdt.base);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_IDTR, env->idt.base);
    if (ret != 0) return ret;

    // Control Registers
    ret = vm_set_register(vcpu, VM_REG_GUEST_CR0, env->cr[0]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_CR2, env->cr[2]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_CR3, env->cr[3]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_CR4, env->cr[4]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_TPR, qcpu->tpr);
    if (ret != 0) return ret;

    // Debug Registers
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR0, env->dr[0]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR1, env->dr[1]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR2, env->dr[2]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR3, env->dr[3]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR6, env->dr[6]);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_DR7, env->dr[7]);
    if (ret != 0) return ret;

    // MSRs
    ret = vm_set_register(vcpu, VM_REG_GUEST_EFER, env->efer);
    if (ret != 0) return ret;
#ifdef TARGET_X86_64
    ret = vm_set_register(vcpu, VM_REG_GUEST_FS_BASE, env->segs[R_FS].base);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_GS_BASE, env->segs[R_GS].base);
    if (ret != 0) return ret;
    ret = vm_set_register(vcpu, VM_REG_GUEST_KGS_BASE, env->kernelgsbase);
    if (ret != 0) return ret;
#endif

    return 0; // Success
}

static int vmm_get_registers(CPUState *cpu) {
    CPUX86State *env = cpu_env(cpu);
    AccelCPUState *qcpu = cpu->accel;
    struct vcpu *vcpu = qcpu->vcpu;
    uint64_t val;
    int ret;

    // General-Purpose Registers
    ret = vm_get_register(vcpu, VM_REG_GUEST_RAX, &val);
    if (ret != 0) return ret;
    env->regs[R_EAX] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RBX, &val);
    if (ret != 0) return ret;
    env->regs[R_EBX] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RCX, &val);
    if (ret != 0) return ret;
    env->regs[R_ECX] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RDX, &val);
    if (ret != 0) return ret;
    env->regs[R_EDX] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RSI, &val);
    if (ret != 0) return ret;
    env->regs[R_ESI] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RDI, &val);
    if (ret != 0) return ret;
    env->regs[R_EDI] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RBP, &val);
    if (ret != 0) return ret;
    env->regs[R_EBP] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RSP, &val);
    if (ret != 0) return ret;
    env->regs[R_ESP] = val;

#ifdef TARGET_X86_64
    ret = vm_get_register(vcpu, VM_REG_GUEST_R8, &val);
    if (ret != 0) return ret;
    env->regs[R_R8] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R9, &val);
    if (ret != 0) return ret;
    env->regs[R_R9] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R10, &val);
    if (ret != 0) return ret;
    env->regs[R_R10] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R11, &val);
    if (ret != 0) return ret;
    env->regs[R_R11] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R12, &val);
    if (ret != 0) return ret;
    env->regs[R_R12] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R13, &val);
    if (ret != 0) return ret;
    env->regs[R_R13] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R14, &val);
    if (ret != 0) return ret;
    env->regs[R_R14] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_R15, &val);
    if (ret != 0) return ret;
    env->regs[R_R15] = val;
#endif

    // RIP and RFLAGS
    ret = vm_get_register(vcpu, VM_REG_GUEST_RIP, &val);
    if (ret != 0) return ret;
    env->eip = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_RFLAGS, &val);
    if (ret != 0) return ret;
    env->eflags = val;

    // Segment Registers (Selectors only)
    ret = vm_get_register(vcpu, VM_REG_GUEST_CS, &val);
    if (ret != 0) return ret;
    env->segs[R_CS].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DS, &val);
    if (ret != 0) return ret;
    env->segs[R_DS].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_ES, &val);
    if (ret != 0) return ret;
    env->segs[R_ES].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_SS, &val);
    if (ret != 0) return ret;
    env->segs[R_SS].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_FS, &val);
    if (ret != 0) return ret;
    env->segs[R_FS].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_GS, &val);
    if (ret != 0) return ret;
    env->segs[R_GS].selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_LDTR, &val);
    if (ret != 0) return ret;
    env->ldt.selector = (uint16_t)val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_TR, &val);
    if (ret != 0) return ret;
    env->tr.selector = (uint16_t)val;

    // Descriptor Table Registers (Base only)
    ret = vm_get_register(vcpu, VM_REG_GUEST_GDTR, &val);
    if (ret != 0) return ret;
    env->gdt.base = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_IDTR, &val);
    if (ret != 0) return ret;
    env->idt.base = val;

    // Control Registers
    ret = vm_get_register(vcpu, VM_REG_GUEST_CR0, &val);
    if (ret != 0) return ret;
    env->cr[0] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_CR2, &val);
    if (ret != 0) return ret;
    env->cr[2] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_CR3, &val);
    if (ret != 0) return ret;
    env->cr[3] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_CR4, &val);
    if (ret != 0) return ret;
    env->cr[4] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_TPR, &val);
    if (ret != 0) return ret;
    qcpu->tpr = val;

    // Debug Registers
    ret = vm_get_register(vcpu, VM_REG_GUEST_DR0, &val);
    if (ret != 0) return ret;
    env->dr[0] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DR1, &val);
    if (ret != 0) return ret;
    env->dr[1] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DR2, &val);
    if (ret != 0) return ret;
    env->dr[2] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DR3, &val);
    if (ret != 0) return ret;
    env->dr[3] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DR6, &val);
    if (ret != 0) return ret;
    env->dr[6] = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_DR7, &val);
    if (ret != 0) return ret;
    env->dr[7] = val;

    // MSRs
    ret = vm_get_register(vcpu, VM_REG_GUEST_EFER, &val);
    if (ret != 0) return ret;
    env->efer = val;

#ifdef TARGET_X86_64
    ret = vm_get_register(vcpu, VM_REG_GUEST_FS_BASE, &val);
    if (ret != 0) return ret;
    env->segs[R_FS].base = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_GS_BASE, &val);
    if (ret != 0) return ret;
    env->segs[R_GS].base = val;

    ret = vm_get_register(vcpu, VM_REG_GUEST_KGS_BASE, &val);
    if (ret != 0) return ret;
    env->kernelgsbase = val;
#endif

    return 0; // Success
}

/* -------------------------------------------------------------------------- */

static int
vmm_io_callback(struct vm_qio *io)
{
    MemTxAttrs attrs = { 0 };
    int ret;

    ret = address_space_rw(&address_space_io, io->port, attrs, io->data,
        io->size, !io->in);
    if (ret != MEMTX_OK) {
        error_report("Bhyve: I/O Transaction Failed "
            "[%s, port=%u, size=%zu]", (io->in ? "in" : "out"),
            io->port, io->size);
    }

    current_cpu->accel->dirty = false;
    return 0;
}

static int
vmm_mem_callback(struct vm_qmem *mem)
{

    address_space_rw(&address_space_memory, mem->gpa, MEMTXATTRS_UNSPECIFIED,
                     mem->data, mem->size, mem->write);

    current_cpu->accel->dirty = false;
    return 0;
}

/* Directly copy from Bhyve */
static int bhyve_intel_rdmsr(uint32_t num, uint64_t* val) {
    int error = 0;
    switch (num) {
    case MSR_BIOS_SIGN:
    case MSR_IA32_PLATFORM_ID:
    case MSR_PKG_ENERGY_STATUS:
    case MSR_PP0_ENERGY_STATUS:
    case MSR_PP1_ENERGY_STATUS:
    case MSR_DRAM_ENERGY_STATUS:
    case MSR_MISC_FEATURE_ENABLES:
        *val = 0;
        break;
    case MSR_RAPL_POWER_UNIT:
        /*
            * Use the default value documented in section
            * "RAPL Interfaces" in Intel SDM vol3.
            */
        *val = 0x000a1003;
        break;
    case MSR_IA32_FEATURE_CONTROL:
        /*
            * Windows guests check this MSR.
            * Set the lock bit to avoid writes
            * to this MSR.
            */
        *val = IA32_FEATURE_CONTROL_LOCK;
        break;
    default:
        error = -1;
        break;
    }
    return error;
}

static int bhyve_amd_rdmsr(uint32_t num, uint64_t* val) {
    int error = 0;
    switch (num) {
    case MSR_BIOS_SIGN:
        *val = 0;
        break;
    case MSR_HWCR:
        /*
            * Bios and Kernel Developer's Guides for AMD Families
            * 12H, 14H, 15H and 16H.
            */
        *val = 0x01000010;	/* Reset value */
        *val |= 1 << 9;		/* MONITOR/MWAIT disable */
        break;

    case MSR_NB_CFG1:
    case MSR_LS_CFG:
    case MSR_IC_CFG:
        /*
            * The reset value is processor family dependent so
            * just return 0.
            */
        *val = 0;
        break;

    case MSR_PERFEVSEL0:
    case MSR_PERFEVSEL1:
    case MSR_PERFEVSEL2:
    case MSR_PERFEVSEL3:
        /*
            * PerfEvtSel MSRs are not properly virtualized so just
            * return zero.
            */
        *val = 0;
        break;

    case MSR_K7_PERFCTR0:
    case MSR_K7_PERFCTR1:
    case MSR_K7_PERFCTR2:
    case MSR_K7_PERFCTR3:
        /*
            * PerfCtr MSRs are not properly virtualized so just
            * return zero.
            */
        *val = 0;
        break;

    case MSR_SMM_ADDR:
    case MSR_SMM_MASK:
        /*
            * Return the reset value defined in the AMD Bios and
            * Kernel Developer's Guide.
            */
        *val = 0;
        break;

    case MSR_P_STATE_LIMIT:
    case MSR_P_STATE_CONTROL:
    case MSR_P_STATE_STATUS:
    case MSR_P_STATE_CONFIG(0):	/* P0 configuration */
        *val = 0;
        break;

    /*
        * OpenBSD guests test bit 0 of this MSR to detect if the
        * workaround for erratum 721 is already applied.
        * https://support.amd.com/TechDocs/41322_10h_Rev_Gd.pdf
        */
    case 0xC0011029:
        *val = 1;
        break;

    default:
        error = -1;
        break;
    }
    return error;
}

/* rdmsr & wrmsr */
static int bhyve_rdmsr(struct vcpu* vcpu, struct vm_exit *vme) {
    struct bhyve_machine* mach = get_bhyve_mach();
    int error;
    uint32_t num = vme->u.msr.code;
    uint64_t val;

    switch (mach->cpu_vendor) {
    case VENDOR_INTEL:
        error = bhyve_intel_rdmsr(num, &val);
        goto finish;
    case VENDOR_AMD:
        error = bhyve_amd_rdmsr(num, &val);
        goto finish;
    case VENDOR_UNKNOWN:
        return -1;
    }

finish:
    error = vm_set_register(vcpu, VM_REG_GUEST_RAX, val);
    error = vm_set_register(vcpu, VM_REG_GUEST_RDX, val >> 32);

    return error;
}

static int bhyve_intel_wrmsr(uint32_t num) {
    switch (num) {
    case 0xd04:		/* Sandy Bridge uncore PMCs */
    case 0xc24:
        return (0);
    case MSR_BIOS_UPDT_TRIG:
        return (0);
    case MSR_BIOS_SIGN:
        return (0);
    default:
        break;
    }
    return 0;
}

static int bhyve_amd_wrmsr(uint32_t num) {
    switch (num) {
    case MSR_HWCR:
        /*
            * Ignore writes to hardware configuration MSR.
            */
        return (0);

    case MSR_NB_CFG1:
    case MSR_LS_CFG:
    case MSR_IC_CFG:
        return (0);	/* Ignore writes */

    case MSR_PERFEVSEL0:
    case MSR_PERFEVSEL1:
    case MSR_PERFEVSEL2:
    case MSR_PERFEVSEL3:
        /* Ignore writes to the PerfEvtSel MSRs */
        return (0);

    case MSR_K7_PERFCTR0:
    case MSR_K7_PERFCTR1:
    case MSR_K7_PERFCTR2:
    case MSR_K7_PERFCTR3:
        /* Ignore writes to the PerfCtr MSRs */
        return (0);

    case MSR_P_STATE_CONTROL:
        /* Ignore write to change the P-state */
        return (0);

    default:
        break;
    }
    return 0;
}

static int bhyve_wrmsr(struct vm_exit *vme) {
    struct bhyve_machine* mach = get_bhyve_mach();
    int error;
    uint32_t num = vme->u.msr.code;
    switch (mach->cpu_vendor) {
    case VENDOR_INTEL:
        error = bhyve_intel_wrmsr(num);
        return error;
    case VENDOR_AMD:
        error = bhyve_amd_wrmsr(num);
        return error;
    case VENDOR_UNKNOWN:
        return -1;
    }

}

/* -------------------------------------------------------------------------- */

static void bhyve_vcpu_pre_run(CPUState *cpu) {
    AccelCPUState *qcpu = cpu->accel;
    struct vcpu *vcpu = qcpu->vcpu;
    X86CPU *x86_cpu = X86_CPU(cpu);
    uint8_t tpr;
    bool sync_tpr = false;
    int ret;

    bql_lock();

    // TPR is always synced (check bhyve-apic)
    tpr = cpu_get_apic_tpr(x86_cpu->apic_state);
    if (tpr != qcpu->tpr) {
        qcpu->tpr = tpr;
        sync_tpr = true;
    }

    /*
     * Force the VCPU out of its inner loop to process any INIT requests
     * or commit pending TPR access.
     */
    if (cpu->interrupt_request & (CPU_INTERRUPT_INIT | CPU_INTERRUPT_TPR)) {
        cpu->exit_request = 1;
    }

    /* Handle NMIs (vmm takes care of nmi windows and interrupt shadows) */
    if (cpu->interrupt_request & CPU_INTERRUPT_NMI) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_NMI;
        printf("Hung here\n");
        vm_inject_nmi(vcpu);
    }

    /* Don't want SMIs. */
    if (cpu->interrupt_request & CPU_INTERRUPT_SMI) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_SMI;
    }

    // Handled By i8259 PIC Device handles emulation

    if (sync_tpr) {
        ret = vm_set_register(vcpu, VM_REG_GUEST_TPR, qcpu->tpr);
    }

    bql_unlock();
}

static void bhyve_vcpu_post_run(CPUState *cpu) {
    CPUX86State *env = cpu_env(cpu);
    X86CPU *x86_cpu = X86_CPU(cpu);
    AccelCPUState *qcpu = cpu->accel;
    struct vcpu *vcpu = qcpu->vcpu;
    uint64_t val;
    int ret;

    // Set Eflags
    ret = vm_get_register(vcpu, VM_REG_GUEST_RFLAGS, &env->eflags);

    // TPR
    ret = vm_get_register(vcpu, VM_REG_GUEST_TPR, &val);
    if (qcpu->tpr != val) {
        qcpu->tpr = val;
        bql_lock();
        cpu_set_apic_tpr(x86_cpu->apic_state, qcpu->tpr);
        bql_unlock();
    }
}

/* vCPU functions */
static int bhyve_vcpu_run(CPUState *cpu) {
    struct bhyve_machine* mach = get_bhyve_mach();
    X86CPU *x86_cpu = X86_CPU(cpu);
    CPUX86State *env = &x86_cpu->env;
    AccelCPUState* qcpu = cpu->accel;

	struct vm_exit vme;
	struct vm_run vmrun;
	int error, rc = 0;
	enum vm_exitcode exitcode;
    enum vm_suspend_how how;
	cpuset_t dmask;

	vmrun.vm_exit = &vme;
	vmrun.cpuset = &dmask;
	vmrun.cpusetsize = sizeof(dmask);

    if (cpu->interrupt_request & CPU_INTERRUPT_INIT) {
        bhyve_cpu_synchronize_state(cpu);
        do_cpu_init(x86_cpu);
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_POLL) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_POLL;
        apic_poll_irq(x86_cpu->apic_state);
    }
    if (((cpu->interrupt_request & CPU_INTERRUPT_HARD) &&
         (env->eflags & IF_MASK)) ||
        (cpu->interrupt_request & CPU_INTERRUPT_NMI)) {
        cpu->halted = false;
    }
    if (cpu->interrupt_request & CPU_INTERRUPT_SIPI) {
        bhyve_cpu_synchronize_state(cpu);
        do_cpu_sipi(x86_cpu);
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_TPR) {
        cpu->interrupt_request &= ~CPU_INTERRUPT_TPR;
        bhyve_cpu_synchronize_state(cpu);
        apic_handle_tpr_access_report(x86_cpu->apic_state, env->eip,
                                      env->tpr_access_type);
    }

    bql_unlock();
    cpu_exec_start(cpu);

	while (rc == 0) {
        if (qcpu->dirty) {
            vmm_set_registers(cpu);
            qcpu->dirty = false;
        }

        bhyve_vcpu_pre_run(cpu);

        smp_rmb();
		error = vm_run(qcpu->vcpu, &vmrun);

        if (error != 0) {
            fprintf(stderr, "Error running vm: %s\n", strerror(error));
            break;
        }

        bhyve_vcpu_post_run(cpu);

		exitcode = vme.exitcode;
        switch (exitcode) {
        case VM_EXITCODE_HLT:
            printf("System Halted\n");
            bql_lock();
            cpu->exception_index = EXCP_HLT;
            cpu->halted = true;
            rc = 1;
            bql_unlock();
            break;
        case VM_EXITCODE_DEBUG:
            usleep(1000);
            dump_registers(qcpu->vcpu);
            break;
        case VM_EXITCODE_INOUT:
        case VM_EXITCODE_INOUT_STR:
            rc = vm_assist_qio(qcpu->vcpu, vmm_io_callback, &vme);
            break;
        case VM_EXITCODE_INST_EMUL:
            rc = vm_assist_qmem(qcpu->vcpu, vmm_mem_callback, &vme);
            break;
        case VM_EXITCODE_RDMSR:
            rc = bhyve_rdmsr(qcpu->vcpu, &vme);
            if (rc) {
                printf("Error Reading to MSR...\n");
                continue;
            }
            break;
        case VM_EXITCODE_WRMSR:
            rc = bhyve_wrmsr(&vme);
            if (rc) {
                printf("Error Writing to MSR...\n");
                continue;
            }
            break;
        case VM_EXITCODE_BOGUS:
            break;
        case VM_EXITCODE_SUSPENDED:
            how = vme.u.suspended.how;

            switch (how) {
            case VM_SUSPEND_RESET:
                exit(0);
            case VM_SUSPEND_POWEROFF:
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                cpu->exception_index = EXCP_INTERRUPT;
                vm_destroy(mach->vm);
                rc = 1;
                break;
            default:
                rc = 1;
            }
            break;
        case VM_EXITCODE_VMX:
            dump_registers(qcpu->vcpu);
            rc = -1;
            break;
        case VM_EXITCODE_SVM:
            rc = -1;
            break;
        case VM_EXITCODE_PAUSE:
            continue;
        case VM_EXITCODE_IPI:
            printf("Received an IPI\n");
            break;
        default:
            printf("Unhandled exit. Register Dump...\n");
            dump_registers(qcpu->vcpu);
            bql_lock();
            qemu_system_guest_panicked(cpu_get_crash_info(cpu));
            bql_unlock();
            break;
        }
	}

    cpu_exec_end(cpu);
    bql_lock();

    qatomic_set(&cpu->exit_request, false);

    return rc;
}
/* End vCPU functions */

static void
bhyve_ipi_signal(int sigcpu)
{
    if (current_cpu) {
        AccelCPUState *qcpu = current_cpu->accel;

        vm_suspend_cpu(qcpu->vcpu);
    }
}

static void
bhyve_init_cpu_signals(void)
{
    struct sigaction sigact;
    sigset_t set;

    /* Install the IPI handler. */
    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = bhyve_ipi_signal;
    sigaction(SIG_IPI, &sigact, NULL);

    /* Allow IPIs on the current thread. */
    sigprocmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIG_IPI);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
}

/* vCPU initialization functions */
int bhyve_init_vcpu(CPUState *cpu)
{
    struct bhyve_machine* mach = get_bhyve_mach();

    AccelCPUState *qcpu;
    int err, tmp;

    qcpu = g_new0(AccelCPUState, 1);
    bhyve_init_cpu_signals();

    // Create vCPU
    qcpu->vcpu = vm_vcpu_open(mach->vm, cpu->cpu_index); // cpu_index 0 is BSP

    err = vm_get_capability(qcpu->vcpu, VM_CAP_HALT_EXIT, &tmp);
    if (err < 0) {
		fprintf(stderr, "Could not set capability halt exit (%d)", err);
    }
    vm_set_capability(qcpu->vcpu, VM_CAP_HALT_EXIT, 1);

    err = vm_set_capability(qcpu->vcpu, VM_CAP_PAUSE_EXIT, 1);
	if (err) {
		fprintf(stderr, "Could not set capability exit (%d)", err);
	}
    err = vm_set_x2apic_state(qcpu->vcpu, X2APIC_DISABLED);
	if (err) {
		fprintf(stderr, "Could not set capability x2apic (%d)", err);
	}
	err = vm_set_capability(qcpu->vcpu, VM_CAP_ENABLE_INVPCID, 1);
	if (err) {
		fprintf(stderr, "Could not set capability Invpcid (%d)", err);
	}

	err = vm_set_capability(qcpu->vcpu, VM_CAP_IPI_EXIT, 1);
	if (err) {
		fprintf(stderr, "Could not set capability IPI Exit (%d)", err);
	}

    // Start vCPU
    if (cpu->cpu_index == 0) { // BSP
        // Can run in real mode
        err = vm_set_capability(qcpu->vcpu,
            VM_CAP_UNRESTRICTED_GUEST, 1);

        err = vcpu_reset(qcpu->vcpu);
        assert(err == 0);
    } else {
        err = vcpu_reset(qcpu->vcpu);
        err = vm_set_capability(qcpu->vcpu,
            VM_CAP_UNRESTRICTED_GUEST, 1);
    }

    /* */
    err = vm_activate_cpu(qcpu->vcpu);
    err = vm_suspend_cpu(qcpu->vcpu);

    // Sync registers on exec
    qcpu->dirty = true;
    cpu->accel = qcpu;


    return 0; // Return success
}

void bhyve_destroy_vcpu(CPUState *cpu) {
    struct bhyve_machine* mach = get_bhyve_mach();

    AccelCPUState *qcpu = cpu->accel;

    vm_vcpu_close(qcpu->vcpu);
}

int bhyve_vcpu_exec(CPUState *cpu)
{
    int ret;
    while (1) {
        if (cpu->exception_index >= EXCP_INTERRUPT) {
            ret = cpu->exception_index;
            cpu->exception_index = -1;
            break;
        }

		vm_resume_cpu(cpu->accel->vcpu);
        ret = bhyve_vcpu_run(cpu);
    }

    return ret;
}

static void
do_bhyve_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    vmm_get_registers(cpu);
    cpu->accel->dirty = true;
}

static void
do_bhyve_cpu_synchronize_post_reset(CPUState *cpu, run_on_cpu_data arg)
{
    vmm_set_registers(cpu);
    cpu->accel->dirty = false;
}

static void
do_bhyve_cpu_synchronize_pre_loadvm(CPUState *cpu, run_on_cpu_data arg)
{
    cpu->accel->dirty = true;
}

/* State Synchronization */
void bhyve_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->accel->dirty) {
        run_on_cpu(cpu, do_bhyve_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

void bhyve_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_bhyve_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void bhyve_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_bhyve_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

void bhyve_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_bhyve_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}
/* End State Syncrhonization */

/* Memory Support */
struct bhyve_seg_and_off {
    struct bhyve_host_seg seg;
    size_t offset;
};

static struct bhyve_seg_and_off calc_segoff_from_vmap(GArray *host_vmap, void* host_va) {
    struct bhyve_seg_and_off ret;
    struct bhyve_host_seg seg;
    for (size_t i = 0; i < host_vmap->len; i++) {
        seg = g_array_index(host_vmap, struct bhyve_host_seg, i);
        if (IN_MEMRANGE((char *)seg.seg_start, seg.size, (char *)host_va)) {
            ret.offset = (char*)host_va - (char*)seg.seg_start;
            ret.seg = seg;
            return ret;
        }
    }

    ret.offset = -1;
    return ret;
}

static void bhyve_update_mapping(hwaddr start_pa, ram_addr_t size,
                                 void *host_va, int add, int rom,
                                 const char *name)
{
    struct bhyve_machine *mach = get_bhyve_mach();
    struct bhyve_seg_and_off segoff;
    int prot, ret;
    segoff = calc_segoff_from_vmap(mach->host_vmap, host_va);

    if (add) {
        prot = PROT_READ | PROT_EXEC;
        if (!rom) {
            prot |= PROT_WRITE;
        }
        ret = vm_mmap_memseg(mach->vm, start_pa, segoff.seg.segid, segoff.offset, size, prot);
    } else {
        ret = vm_munmap_memseg(mach->vm, start_pa, size);
    }
}

static void bhyve_process_section(MemoryRegionSection *section, int add)
{
    MemoryRegion *mr = section->mr;
    hwaddr start_pa = section->offset_within_address_space;
    ram_addr_t size = int128_get64(section->size);
    unsigned int delta;
    uint64_t host_va;

    // Even Bios and ROM is backed by ram (except UEFI mode)
    if (!memory_region_is_ram(mr) && strcmp(mr->name, "system.flash0") && strcmp(mr->name, "system.flash1")) {
        return;
    }

    // Ensure MemoryRegion is aligned on page boundary and size is multiple of page size
    delta = qemu_real_host_page_size() - (start_pa & ~qemu_real_host_page_mask());
    delta &= ~qemu_real_host_page_mask();
    if (delta > size) {
        return;
    }
    start_pa += delta;
    size -= delta;
    size &= qemu_real_host_page_mask();
    if (!size || (start_pa & ~qemu_real_host_page_mask())) {
        return;
    }

    host_va = (uintptr_t)memory_region_get_ram_ptr(mr) +
        section->offset_within_region + delta;

    bhyve_update_mapping(start_pa, size, (void*)(uintptr_t)host_va, add,
        memory_region_is_rom(mr), mr->name);
    return;
}

static void bhyve_region_add(MemoryListener *listener,
                             MemoryRegionSection *section)
{
    memory_region_ref(section->mr);
    bhyve_process_section(section, 1);
}

static void bhyve_region_del(MemoryListener *listener,
                             MemoryRegionSection *section)
{
    bhyve_process_section(section, 0);
    memory_region_unref(section->mr);
}

static void bhyve_transaction_begin(MemoryListener *listener)
{
}

static void bhyve_transaction_commit(MemoryListener *listener)
{
}

static void bhyve_log_sync(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!memory_region_is_ram(mr)) {
        return;
    }

    memory_region_set_dirty(mr, 0, int128_get64(section->size));
}

static MemoryListener bhyve_memory_listener = {
    .name = "bhyve",
    .begin = bhyve_transaction_begin,
    .commit = bhyve_transaction_commit,
    .region_add = bhyve_region_add,
    .region_del = bhyve_region_del,
    .log_sync = bhyve_log_sync,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};

static void bhyve_memory_init(void)
{
    memory_listener_register(&bhyve_memory_listener, &address_space_memory);
}

/* Allocate memory */
static void* bhyve_allocate_pc_memory(size_t mr_size, const char* name, int segid) {
    struct bhyve_machine *mach = get_bhyve_mach();
    char* baseaddr;
    int err;

    err = vm_setup_qmemory(mach->vm, mr_size, segid, VM_MMAP_ALL, name);
    if (err < 0) {
        fprintf(stderr, "Couldn't setup PC Memory\n");
        exit(4);
    }

    err = vm_get_guestmem_from_ctx(mach->vm, &baseaddr, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "Couldn't access machine baseaddr\n");
        exit(4);
    }
    return baseaddr;
}

static char *bhyve_serialize_name(const char *input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    char *temp = malloc(len + 1);  // Temporary buffer
    if (!temp) return NULL;

    // Remove invalid characters
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (input[i] != '/') {
            temp[j++] = input[i];
        }
    }
    temp[j] = '\0';

    // Compute start index for last 15 characters
    size_t final_len = j < 15 ? j : 15;
    const char *start = temp + (j > 15 ? (j - 15) : 0);

    // Allocate final output
    char *output = malloc(final_len + 1);
    if (!output) {
        free(temp);
        return NULL;
    }

    strncpy(output, start, final_len);
    output[final_len] = '\0';

    free(temp);
    return output;
}


void *bhyve_ram_alloc(size_t mr_size, uint64_t *alignment, int flags, const char* name) {
    struct bhyve_machine *mach = get_bhyve_mach();
    struct bhyve_host_seg ram_memseg;
    ram_memseg.mmap_flags = MAP_SHARED | MAP_NORESERVE | MAP_FIXED;
    ram_memseg.segid = segid_num;
    ram_memseg.size = mr_size;
    ram_memseg.name = bhyve_serialize_name(name);
    puts(name);


    if (strcmp(name, "pc.ram") == 0) {
        ram_memseg.seg_start = bhyve_allocate_pc_memory(mr_size, ram_memseg.name, segid_num++);
    } else {
        ram_memseg.seg_start = vm_create_devmem(mach->vm, segid_num++, ram_memseg.name, mr_size);
    }
    if (!ram_memseg.seg_start || ram_memseg.seg_start == (void*)SIZE_MAX) {
        fprintf(stderr, "Could not allocate device memory for %s\n", name);
        exit(4);
    }
    *alignment = SUPERPAGE_SIZE;
    g_array_append_val(mach->host_vmap, ram_memseg);
    return ram_memseg.seg_start;

}
/* End Memory Support */

/* Kernel IRQchip Processing */
static void bhyve_set_kernel_irqchip(Object *obj, Visitor *v,
                                   const char *name, void *opaque,
                                   Error **errp)
{
    struct bhyve_machine *mach = &bhyve_mach;
    OnOffSplit mode;

    if (!visit_type_OnOffSplit(v, name, &mode, errp)) {
        return;
    }

    switch (mode) {
    case ON_OFF_SPLIT_ON:
        /* In kernel LAPIC */
        mach->kernel_irqchip_required = true;
        break;

    case ON_OFF_SPLIT_OFF:
        /* QEMU userspace emulated LAPIC */
        mach->kernel_irqchip_required = false;
        break;

    case ON_OFF_SPLIT_SPLIT:
        error_setg(errp, "Bhyve: split irqchip currently not supported");
        error_append_hint(errp,
            "Try without kernel-irqchip or with kernel-irqchip=on|off");
        break;

    default:
        /*
         * The value was checked in visit_type_OnOffSplit() above. If
         * we get here, then something is wrong in QEMU.
         */
        abort();
    }
}

bool bhyve_apic_in_platform(void) {
    return bhyve_mach.kernel_irqchip_required;
}
/* End Kernel IRQchip Processing */

static int do_open(const char *vmname, MachineState* ms) {
    /* Temporary Machine Initialization*/
    struct bhyve_machine* mach = get_bhyve_mach();
    int err;
  
    // Close last VM if it exists
    if ((mach->vm = vm_open(vmname))) {
        vm_destroy(mach->vm);
    }

    mach->vm = vm_openf(vmname, VMMAPI_OPEN_CREATE);

    // Set Topology
    err = vm_set_flags(mach->vm, VM_OP_F_QEMU);
    err = vm_set_topology(mach->vm, ms->smp.sockets, ms->smp.cores, ms->smp.threads, 0);

    vm_set_memflags(mach->vm, 0);

    /* End Memory */
    return err;
}


static int
bhyve_accel_init(AccelState *as, MachineState *ms)
{
    int err;
    printf("Bhyve Accelerator Machine Initialization\n");

    err = do_open(VM_NAME, ms);

    /* Setup Memory */
    bhyve_memory_init();
    return 0;
}

static void
bhyve_accel_class_init(ObjectClass *oc, const void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "Bhyve";
    ac->init_machine = bhyve_accel_init;
    ac->allowed = &bhyve_allowed;

    object_class_property_add(oc, "kernel-irqchip", "on|off|split",
        NULL, bhyve_set_kernel_irqchip,
        NULL, NULL);
    object_class_property_set_description(oc, "kernel-irqchip",
        "Configure Bhyve in-kernel irqchip");
}

static void bhyve_accel_instance_init(Object *obj)
{
    struct bhyve_machine *bhyvep = &bhyve_mach;

    memset(bhyvep, 0, sizeof(struct bhyve_machine));
    bhyvep->host_vmap = g_array_new(FALSE, FALSE, sizeof(struct bhyve_host_seg));

    /* Turn off kernel-irqchip, by default */
    bhyvep->kernel_irqchip_required = true;
    bhyvep->cpu_vendor = get_cpu_vendor();
}

static const TypeInfo bhyve_accel_type = {
    .name = ACCEL_CLASS_NAME("bhyve"),
    .parent = TYPE_ACCEL,
    // Initializes instance specific data
    .instance_init = bhyve_accel_instance_init,
    // Called once when first loaded
    .class_init = bhyve_accel_class_init,
};

static void
bhyve_type_init(void)
{
    type_register_static(&bhyve_accel_type);
}

type_init(bhyve_type_init);
