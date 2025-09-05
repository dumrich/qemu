#ifndef QEMU_BHYVE_H
#define QEMU_BHYVE_H

#include "exec/cpu-common.h"

#ifdef COMPILING_PER_TARGET
# ifdef CONFIG_BHYVE
#  define CONFIG_BHYVE_IS_POSSIBLE
# endif
#else
# define CONFIG_BHYVE_IS_POSSIBLE
#endif /* COMPILING_PER_TARGET */

#ifdef CONFIG_BHYVE_IS_POSSIBLE
int bhyve_enabled(void);
#else /* !CONFIG_BHYVE_IS_POSSIBLE */
#define bhyve_enabled() (0)
#endif /* CONFIG_BHYVE_IS_POSSIBLE */

bool bhyve_apic_in_platform(void);

int bhyve_name2segid(const char* name);
void *bhyve_ram_alloc(size_t mr, uint64_t* alignment, int flags, const char* name);

struct vcpu* bhyve_get_vcpu(CPUState* cpu);
#endif /* QEMU_BHYVE_H */
