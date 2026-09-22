# Compile generic FatFs only; no Zephyr runtime or vendor diskio.c.
add_languages('c', native: false)
fatfs_config = ['-include', meson.current_source_dir() / 'config.h']
fatfs_inc = include_directories('../../third_party/fatfs/include')
fatfs_lib = static_library('daveos-fatfs', '../../third_party/fatfs/ff.c', '../../third_party/fatfs/option/ffunicode.c',
  include_directories: fatfs_inc, c_args: fatfs_config,
  override_options: ['c_std=c11', 'warning_level=0'])
fatfs_dep = declare_dependency(link_with: fatfs_lib, include_directories: fatfs_inc,
  compile_args: fatfs_config)
storage_lib = static_library('daveos-storage', 'volume.cpp', dependencies: [core_dep, fatfs_dep])
storage_dep = declare_dependency(link_with: storage_lib, dependencies: [core_dep, fatfs_dep])
meson.override_dependency('daveos-storage', storage_dep)
