update_lib = static_library('daveos-update', 'package.cpp', 'writer.cpp', 'engine.cpp', 'protocol.cpp', 'file.cpp',
  dependencies: boot_dep)
update_dep = declare_dependency(link_with: update_lib, dependencies: boot_dep)
meson.override_dependency('daveos-update', update_dep)
