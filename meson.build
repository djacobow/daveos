project('daveos', 'cpp', version: '0.1.0', meson_version: '>=1.3',
  default_options: ['cpp_std=c++20', 'cpp_eh=none', 'warning_level=3', 'buildtype=debug'])

python = import('python').find_installation('python3')
platform = get_option('platform')
board = get_option('board')
if platform == 'stm32'
  if not meson.is_cross_build() or host_machine.cpu_family() != 'arm'
    error('STM32 requires the meson/stm32.ini ARM cross file')
  endif
  add_languages('c', native: false)
  # Select one CPU/ABI consistently for all libraries and the application.
  # The H755's sleeping M4 overrides these flags on its own target.
  if board == 'h563'
    platform = 'stm32h5'
    cpu_flags = ['-mcpu=cortex-m33', '-mfpu=fpv5-sp-d16']
  else
    platform = 'stm32h7'
    cpu_flags = ['-mcpu=cortex-m7', '-mfpu=fpv5-d16']
  endif
  cpu_flags += ['-mthumb', '-mfloat-abi=hard']
  add_project_arguments(cpu_flags + ['-ffreestanding', '-ffunction-sections',
    '-fdata-sections'], language: ['c', 'cpp'])
  add_project_link_arguments(cpu_flags + ['--specs=nosys.specs'], language: ['c', 'cpp'])
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
