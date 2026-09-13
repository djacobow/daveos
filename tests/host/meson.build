exe = executable('test-host', '../host.cc', dependencies: [host_dep, fake_dep, catch_dep])
test('host', exe, suite: 'integration', timeout: 30)
