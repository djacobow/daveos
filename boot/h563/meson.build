boot_sources = files('main.cpp', '../../platform/stm32h5/fault_handlers.S',
  '../../platform/stm32/nucleo/h563/startup_stm32h563xx.s',
  '../../platform/stm32/nucleo/h563/Core/Src/system_stm32h5xx.c')
boot_dependencies = [boot_dep, stm32h5_dep, stm32h5_cmsis_dep, boot_version_dep]
boot_compile_args = ['-fno-rtti', '-fno-freestanding', '-D_GLIBCXX_NO_ASSERTIONS']
boot_link_args = cpu_flags + ['--specs=nosys.specs', '--specs=nano.specs', '-Wl,--gc-sections', '-Wl,--print-memory-usage']
subdir('probe')
boot_layout_tool = files('../../tools/boot_layout.py')
boot_layout = custom_target('boot-layout', input: boot_probe,
  output: ['layout.h', 'boot.ld', 'slot_a.ld', 'slot_b.ld', 'layout.json'],
  command: [python, boot_layout_tool, '--probe', '@INPUT@',
    '--source-linker', board_linker_path, '--directory', meson.current_build_dir()],
  depend_files: board_linker_script)
boot_firmware = executable('bootloader', boot_sources, boot_layout[0],
  dependencies: boot_dependencies, include_directories: include_directories('.'),
  cpp_args: boot_compile_args,
  link_args: boot_link_args + ['-T' + meson.current_build_dir() / 'boot.ld',
    '-Wl,-Map=' + meson.current_build_dir() / 'bootloader.map'],
  link_depends: boot_layout[1], name_suffix: 'elf')
boot_layout_dep = declare_dependency(sources: boot_layout[0],
  include_directories: include_directories('.'), dependencies: [boot_dep, update_dep])
boot_layout_directory = meson.current_build_dir()
boot_size_check = custom_target('boot-size-check', input: [boot_probe, boot_firmware],
  output: 'boot-size.txt', capture: true, build_by_default: true,
  command: [python, boot_layout_tool, '--probe', '@INPUT0@', '--check', '@INPUT1@',
    '--source-linker', board_linker_path, '--directory', meson.current_build_dir()])
boot_binary = custom_target('bootloader-bin', input: boot_firmware,
  output: 'bootloader.bin', command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
  build_by_default: true)
