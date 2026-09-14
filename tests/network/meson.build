exe = executable('test-network', 'network.cpp',
  dependencies: [test_support_dep, network_dep], include_directories: network_includes,
  link_args: ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free'])
test('network', exe, suite: 'unit', timeout: 20)
