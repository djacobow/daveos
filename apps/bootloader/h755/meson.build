boot_layout_tool = files('../../../tools/boot_layout.py')
boot_layout = custom_target('boot-layout',
  output: ['layout.h', 'boot.ld', 'slot_a.ld', 'slot_b.ld', 'layout.json'],
  command: [python, boot_layout_tool, '--board', 'h755',
    '--source-linker', board_linker_path, '--directory', meson.current_build_dir()],
  depend_files: board_linker_script)
boot_firmware = executable('bootloader', 'main.cpp', boot_layout[0],
  '../../../platform/stm32h7/fault_handlers.S',
  '../../../platform/stm32/nucleo/h755/CM7/Core/Startup/startup_stm32h755xx_CM7.s',
  '../../../platform/stm32/nucleo/h755/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c',
  dependencies: [stm32_memory_dep, boot_dep, stm32h7_dep, boot_version_dep],
  include_directories: include_directories('.'),
  cpp_args: ['-DCORE_CM7', '-fno-rtti', '-fno-freestanding', '-D_GLIBCXX_NO_ASSERTIONS'],
  c_args: ['-DCORE_CM7', '-DUSE_PWR_DIRECT_SMPS_SUPPLY', '-DUSER_VECT_TAB_ADDRESS'],
  link_args: cpu_flags + ['--specs=nosys.specs', '--specs=nano.specs', '-Wl,--gc-sections',
    '-Wl,--print-memory-usage', '-T' + meson.current_build_dir() / 'boot.ld',
    '-Wl,-Map=' + meson.current_build_dir() / 'bootloader.map'],
  link_depends: boot_layout[1], name_suffix: 'elf')
boot_layout_dep = declare_dependency(sources: boot_layout[0],
  include_directories: include_directories('.'), dependencies: [stm32_memory_dep, boot_dep, update_dep])
boot_layout_directory = meson.current_build_dir()
boot_size_check = custom_target('boot-size-check', input: boot_firmware,
  output: 'boot-size.txt', capture: true, build_by_default: true,
  command: [python, boot_layout_tool, '--board', 'h755', '--check', '@INPUT@',
    '--source-linker', board_linker_path, '--directory', meson.current_build_dir()])
boot_binary = custom_target('bootloader-bin', input: boot_firmware,
  output: 'bootloader.bin', command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
  build_by_default: true)

memory_audit_images += [boot_firmware]
