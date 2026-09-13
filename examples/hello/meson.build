foreach adapter, dep : example_platforms
  exe = executable('hello-' + adapter, 'hello.cc',
    cpp_args: adapter == 'fake' ? ['-DDAVEOS_FAKE'] : [], dependencies: dep)
  test('hello-' + adapter, exe, suite: 'smoke', timeout: 10)
endforeach
