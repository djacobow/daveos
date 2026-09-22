exe = executable('test-network', 'network.cpp', package_fixture,
  cpp_args: ['-DREFERENCE_PACKAGE="' + meson.project_build_root() / 'tests/reliability/reference.ota' + '"'],
  dependencies: [test_support_dep, network_dep, console_dep, network_update_dep], include_directories: network_includes,
  link_args: ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free'])
test('network', exe, suite: 'unit', timeout: 20)
