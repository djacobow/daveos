# Keep downloaded sources in build/deps, not in the source tree.
catch_dir = run_command(python, files('../../tools/dependencies.py'), check: true).stdout().strip()
catch_lib = static_library('catch2', catch_dir / 'catch_amalgamated.cpp',
  cpp_args: ['-DCATCH_CONFIG_DISABLE_EXCEPTIONS'], override_options: ['warning_level=0'])
catch_dep = declare_dependency(link_with: catch_lib,
  include_directories: include_directories(import('fs').relative_to(catch_dir, meson.current_source_dir())),
  compile_args: ['-DCATCH_CONFIG_DISABLE_EXCEPTIONS'])
