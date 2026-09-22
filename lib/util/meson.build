version_parts = meson.project_version().split('.')
version_header = custom_target('version-stamp', output: ['version_stamp.h', 'version.json'],
  command: [python, files('../../tools/version.py'), '--root', meson.project_source_root(),
    '--major', version_parts[0], '--minor', version_parts[1],
    '--build', get_option('version_build'), '--output', '@OUTPUT0@', '--json', '@OUTPUT1@'],
  build_always_stale: true)
util_lib = static_library('daveos-util', 'fault.cpp', dependencies: core_dep)
util_dep = declare_dependency(link_with: util_lib, include_directories: include_directories('..'),
  sources: version_header[0])
meson.override_dependency('daveos-util', util_dep)

boot_version_header = custom_target('boot-version-stamp',
  output: ['boot_version_stamp.h', 'boot_version.json'],
  command: [python, files('../../tools/version.py'), '--root', meson.project_source_root(),
    '--component', 'bootloader', '--major', get_option('boot_version_major').to_string(),
    '--minor', get_option('boot_version_minor').to_string(),
    '--build', get_option('version_build'), '--output', '@OUTPUT0@', '--json', '@OUTPUT1@'],
  build_always_stale: true)
boot_version_dep = declare_dependency(sources: boot_version_header[0], dependencies: core_dep)
