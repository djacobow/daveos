project('daveos', 'cpp', version: '0.1.0', meson_version: '>=1.3',
  default_options: ['cpp_std=c++20', 'cpp_eh=none', 'warning_level=3', 'buildtype=debug'])

python = import('python').find_installation('python3')
platform = get_option('platform')
if platform in ['stm32h5', 'stm32h7']
  if not meson.is_cross_build()
    error('STM32 platforms require a matching Meson cross file')
  endif
  add_languages('c', native: false)
endif

subdir('tools')
subdir('core')
subdir('console')
if get_option('networking')
  subdir('net')
endif
subdir('platform')
firmware_images = []
if get_option('examples')
  subdir('examples')
endif
if firmware_images.length() > 0
  subdir('tools/programming')
endif
if platform in ['stm32h5', 'stm32h7']
  subdir('tests/arm')
elif get_option('tests')
  subdir('tests')
endif
