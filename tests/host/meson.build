exe = executable('test-host', 'host.cpp', dependencies: [host_dep, test_support_dep])
test('host', exe, suite: 'integration', timeout: 30)
io = executable('test-host-io', 'io.cpp', dependencies: fake_dep)
test('host-io', python, args: [files('io.py'), io, get_option('logging') ? 'true' : 'false'],
  suite: 'integration')
