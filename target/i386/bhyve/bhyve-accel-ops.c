#include <string.h>
#include <unistd.h>
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "system/accel-ops.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"

#include "system/bhyve.h"
#include "bhyve-accel-ops.h"

static void *qemu_bhyve_cpu_thread_fn(void *arg) {
    CPUState *cpu = arg;
    int r;

    // Verify Bhyve is enabled
    assert(bhyve_enabled());

    // RCU subsystem needs to be aware of all threads that can act as RCU readers
    rcu_register_thread();

    // Acquire Big Qemu Lock
    bql_lock();

    qemu_thread_get_self(cpu->thread);
    // Set the CPUState thread ID to the Posix TID
    cpu->thread_id = qemu_get_thread_id();
    current_cpu = cpu;

    /* TODO: Initialize vCPU */
    r = bhyve_init_vcpu(cpu);
    if (r < 0) {
        fprintf(stderr, "bhyve_init_vcpu failed: %s\n", strerror(-r));
    }
    /* End Initialize */

    cpu_thread_signal_created(cpu);
    // Deterministic mode seed
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* vCPU Loop */
    do {
        if (cpu_can_run(cpu)) {
            r = bhyve_vcpu_exec(cpu);
        }
        while (cpu_thread_is_idle(cpu)) {
            qemu_cond_wait_bql(cpu->halt_cond);
        }
        qemu_wait_io_event_common(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));
    /* End vCPU Loop */


    bhyve_destroy_vcpu(cpu);
    cpu_thread_signal_destroyed(cpu);
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

// Dummy implementations for demonstration
static void bhyve_start_vcpu_thread(CPUState *cpu) {
    char thread_name[VCPU_THREAD_NAME_SIZE];

    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/Bhyve",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, qemu_bhyve_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}

static void bhyve_kick_vcpu_thread(CPUState *cpu) {
    cpu->exit_request = 1;
    cpus_kick_thread(cpu);
}


static void bhyve_accel_ops_class_init(ObjectClass *oc, const void *data)
{
    AccelOpsClass *ops = ACCEL_OPS_CLASS(oc);

    // VCPU Thread Management
    ops->create_vcpu_thread = bhyve_start_vcpu_thread;
    ops->kick_vcpu_thread = bhyve_kick_vcpu_thread;

    // CPU State Synchronization
    ops->synchronize_post_reset = bhyve_cpu_synchronize_post_reset;
    ops->synchronize_post_init = bhyve_cpu_synchronize_post_init;
    ops->synchronize_state = bhyve_cpu_synchronize_state;
    ops->synchronize_pre_loadvm = bhyve_cpu_synchronize_pre_loadvm;
}

static const TypeInfo bhyve_accel_ops_type = {
    .name = ACCEL_OPS_NAME("bhyve"), // Changed to "bhyve"

    .parent = TYPE_ACCEL_OPS,
    .class_init = bhyve_accel_ops_class_init,
    .abstract = true,
};

static void bhyve_accel_ops_register_types(void)
{
    type_register_static(&bhyve_accel_ops_type);
}

type_init(bhyve_accel_ops_register_types);
