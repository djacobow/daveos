exe = executable('console-host', 'console.cpp', dependencies: host_dep)
test('console-host', python, args: ['-B', '-m', 'pytest', '-q', files('../../tests/host/test_console.py'), '--test-binary', exe.full_path(), '--test-logging', get_option('logging') ? 'enabled' : 'disabled'],
  suite: 'smoke', timeout: 15, depends: exe)
