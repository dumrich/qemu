bhyve_inc = declare_dependency(
  include_directories: include_directories(
    '../../..',
    '../../../include',
  )
)

i386_system_ss.add(when: 'CONFIG_BHYVE', if_true: [
  files(
    'bhyve-all.c',
    'bhyve-i8259.c',
    'bhyve-apic.c',
    'bhyve-ioapic.c',
    'bhyve-accel-ops.c',
  ),
  bhyve_inc,
])
i386_system_ss.add(when: 'CONFIG_BHYVE', if_true: bhyve)
