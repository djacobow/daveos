otp_lib = static_library('daveos-otp', ['record.cpp', 'store.cpp', 'flash.cpp'], dependencies: core_dep)
otp_dep = declare_dependency(link_with: otp_lib, dependencies: core_dep)
meson.override_dependency('daveos-otp', otp_dep)
