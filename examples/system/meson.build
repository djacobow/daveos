foreach adapter, dep : example_platforms
  exe = executable('system-' + adapter, 'system.cc',
    cpp_args: adapter == 'fake' ? ['-DDAVEOS_FAKE'] : [], dependencies: dep)
  test('system-' + adapter, exe, suite: 'smoke', timeout: 10)
endforeach
