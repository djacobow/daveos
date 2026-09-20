boot_probe = executable('boot-probe', boot_sources,
  dependencies: boot_dependencies, include_directories: include_directories('.'),
  cpp_args: boot_compile_args,
  link_args: boot_link_args + ['-T' + board_linker_path],
  link_depends: board_linker_script, name_suffix: 'elf')
