project('daveos', 'cpp', version: '0.1.0', meson_version: '>=1.3',
  default_options: ['cpp_std=c++20', 'cpp_eh=none', 'warning_level=3', 'buildtype=debug'])

python = import('python').find_installation('python3')
platform = get_option('platform')
if platform == 'stm32h5'
  if not meson.is_cross_build()
    error('stm32h5 requires --cross-file meson/stm32h563.ini')
  endif
  add_languages('c', native: false)
  subdir('platform/stm32h5')
endif

subdir('tools')
subdir('src/core')
subdir('src/platform')
if get_option('examples')
  subdir('examples')
endif
if platform == 'stm32h5'
  subdir('tests/arm')
elif get_option('tests')
  subdir('tests')
endif
