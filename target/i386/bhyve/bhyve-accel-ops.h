#ifndef TARGET_I386_BHYVE_ACCEL_OPS_H
#define TARGET_I386_BHYVE_ACCEL_OPS_H

#include "cpu.h"
#include "system/cpus.h"

int bhyve_init_vcpu(CPUState *cpu);
int bhyve_vcpu_exec(CPUState *cpu);
void bhyve_destroy_vcpu(CPUState *cpu);
void bhyve_vcpu_kick(CPUState *cpu);

void bhyve_cpu_synchronize_state(CPUState *cpu);
void bhyve_cpu_synchronize_post_reset(CPUState *cpu);
void bhyve_cpu_synchronize_post_init(CPUState *cpu);
void bhyve_cpu_synchronize_pre_loadvm(CPUState *cpu);

#endif /* TARGET_I386_BHYVE_ACCEL_OPS_H */
