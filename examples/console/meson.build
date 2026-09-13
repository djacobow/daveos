exe = executable('console-host', 'console.cc', dependencies: host_dep)
test('console-host', python, args: [files('smoke.py'), exe.full_path(), get_option('logging') ? 'enabled' : 'disabled'],
  suite: 'smoke', timeout: 15, depends: exe)
