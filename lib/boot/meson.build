boot_lib = static_library('daveos-boot', 'journal.cpp', 'control.cpp', dependencies: core_dep)
boot_dep = declare_dependency(link_with: boot_lib, dependencies: [core_dep, util_dep])
meson.override_dependency('daveos-boot', boot_dep)
