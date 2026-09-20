exe = executable('test-host', 'host.cpp', dependencies: [host_dep, test_support_dep])
test('host', exe, suite: 'integration', timeout: 30)
io = executable('test-host-io', 'io.cpp', dependencies: fake_dep)
test('host-io', python, args: ['-B', '-m', 'pytest', '-q', files('test_io.py'), '--test-binary', io.full_path(), '--test-logging', get_option('logging') ? 'enabled' : 'disabled'],
  suite: 'integration')
