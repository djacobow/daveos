# Keep downloaded sources in build/deps, not in the source tree.
catch_dir = run_command(python, files('../tools/dependencies.py'), check: true).stdout().strip()
catch_lib = static_library('catch2', catch_dir / 'catch_amalgamated.cpp',
  cpp_args: ['-DCATCH_CONFIG_DISABLE_EXCEPTIONS'], override_options: ['warning_level=0'])
catch_dep = declare_dependency(link_with: catch_lib,
  include_directories: include_directories(import('fs').relative_to(catch_dir, meson.current_source_dir())),
  compile_args: ['-DCATCH_CONFIG_DISABLE_EXCEPTIONS'])
test_names = ['core', 'timers', 'logging']
# TSan supplies strong new/delete interceptors; allocation instrumentation is
# validated in ordinary and ASan builds instead.
if not get_option('b_sanitize').contains('thread')
  test_names += ['allocation']
endif
exe = executable('test-stm32h5', 'stm32h5.cc', '../src/platform/stm32h5/platform.cc',
  include_directories: include_directories('stm32h5'),
  dependencies: [core_dep, catch_dep])
test('stm32h5', exe, suite: 'unit', timeout: 20)
foreach name : test_names
  exe = executable('test-' + name, name + '.cc', dependencies: [fake_dep, catch_dep])
  test(name, exe, suite: 'unit', timeout: 20)
endforeach
if platform == 'host'
  exe = executable('test-host', 'host.cc', dependencies: [host_dep, fake_dep, catch_dep])
  test('host', exe, suite: 'integration', timeout: 30)
endif
