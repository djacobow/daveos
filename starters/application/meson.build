project('my-daveos-app', 'cpp', default_options: ['cpp_std=c++20', 'warning_level=3'])

# Meson fetches the wrap once; application targets use only declared dependencies.
host = dependency('daveos-host', fallback: ['daveos', 'host_dep'],
  default_options: ['platform=host', 'tests=false', 'examples=false'])
fake = dependency('daveos-fake', fallback: ['daveos', 'fake_dep'])
executable('my-app', 'main.cpp', dependencies: host)
unit = executable('counter-test', 'counter_test.cpp', dependencies: fake)
test('counter', unit)
