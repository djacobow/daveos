project('daveos', 'cpp', version: '0.1.0', meson_version: '>=1.3',
  default_options: ['cpp_std=c++20', 'cpp_eh=none', 'warning_level=3', 'buildtype=debug'])

python = import('python').find_installation('python3')
foreach check : ['format', 'format-check', 'lint']
  run_target(check, command: [python, files('tools/check.py'), check])
endforeach

headers = include_directories('include')
core_dep = declare_dependency(include_directories: headers)
platform = get_option('platform')
if platform == 'stm32h5'
  if not meson.is_cross_build()
    error('stm32h5 requires --cross-file meson/stm32h563.ini')
  endif
  static_library('daveos-arm-core-check', 'tests/arm_compile.cc', dependencies: core_dep)
  summary('STM32H5 support', 'core compile check only; HAL and firmware linking deferred')
else
  threads = dependency('threads')
  fake_lib = static_library('daveos-fake', 'src/platform/fake.cc', dependencies: [core_dep, threads])
  fake_dep = declare_dependency(link_with: fake_lib, dependencies: [core_dep, threads])
  if platform == 'host'
    host_lib = static_library('daveos-host', 'src/platform/host.cc', dependencies: [core_dep, threads])
    host_dep = declare_dependency(link_with: host_lib, dependencies: [core_dep, threads])
  endif
  if get_option('examples')
    foreach example : ['hello', 'system']
      exe = executable(example + '-fake', 'examples/' + example + '.cc',
        cpp_args: ['-DDAVEOS_FAKE'], dependencies: fake_dep)
      test(example + '-fake', exe, suite: 'smoke', timeout: 10)
      if platform == 'host'
        exe = executable(example + '-host', 'examples/' + example + '.cc', dependencies: host_dep)
        test(example + '-host', exe, suite: 'smoke', timeout: 10)
      endif
    endforeach
  endif
  if get_option('tests')
    subdir('tests')
  endif
endif
