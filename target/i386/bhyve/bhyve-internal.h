#ifndef TARGET_I386_BHYVE_INTERNAL_H
#define TARGET_I386_BHYVE_INTERNAL_H

#include <stdbool.h>
#include <glib.h>

#include <vmmapi.h>

#define SUPERPAGE_SIZE (1 << 21)

#define IN_MEMRANGE(host, range, test) ((test >= host) && (test < (host + range)))

enum host_vendor {
    VENDOR_UNKNOWN,
    VENDOR_INTEL,
    VENDOR_AMD,
};

struct bhyve_host_seg {
    int segid;  // VM Segid
    size_t size;
    void* seg_start;

    int mmap_flags;
    char* name;
};

struct bhyve_machine {
    struct vmctx* vm;

    GArray* host_vmap;
    bool kernel_irqchip_required;

    // Track CPU vendor for MSR support
    enum host_vendor cpu_vendor;
};

extern struct bhyve_machine bhyve_mach;

/* Curent segment ID (incremented on every vm_create_devmem)*/
extern int segid_num;

#endif /* TARGET_I386_BHYVE_INTERNAL_H */
