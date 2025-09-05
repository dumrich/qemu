i386_system_ss.add(when: 'CONFIG_BHYVE', if_true:
  files(
  'bhyve-all.c',
  'bhyve-i8259.c',
  'bhyve-apic.c',
  'bhyve-ioapic.c',
  'bhyve-accel-ops.c',
  )
)

i386_system_ss.add(when: 'CONFIG_BHYVE', if_true: bhyve)

bhyve_includes = include_directories(
  '/home/chabi/freebsd-src/sys',
  '/home/chabi/freebsd-src/sys/amd64',
  '/home/chabi/freebsd-src/sys/amd64/vmm'
)

i386_system_ss.add(
  files('/home/chabi/freebsd-src/sys/amd64/vmm/vmm_instruction_emul.c') 
)