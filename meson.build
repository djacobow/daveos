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
  cpu_flags += ['-mthumb', '-mfloat-abi=hard', '-Os', '-g3']
  add_project_arguments(cpu_flags + ['-ffreestanding', '-ffunction-sections',
    '-fdata-sections'], language: ['c', 'cpp'])
  # Apply to libraries too: GCC's assertion reporter pulls in allocating
  # exception/stdio machinery even with C++ exceptions disabled. Our explicit
  # validation and the existing C-assert policy are unchanged.
  add_project_arguments(['-U_GLIBCXX_ASSERTIONS', '-D_GLIBCXX_NO_ASSERTIONS'], language: 'cpp')
  add_project_link_arguments(cpu_flags, language: ['c', 'cpp'])
endif

if get_option('otp_backend') != 'none' and (platform != 'stm32h5' or not get_option('bootloader'))
  error('OTP console backends require STM32 H563 with bootloader=true')
endif

if get_option('otp_programming') and get_option('otp_backend') != 'h563'
  error('otp_programming requires otp_backend=h563')
endif

subdir('tools')
subdir('core')
subdir('hal')
subdir('drivers')
if get_option('fatfs')
  subdir('storage')
endif
subdir('util')
subdir('boot')
subdir('update')
subdir('otp')
subdir('console')
if get_option('networking')
  subdir('net')
endif
firmware_images = []
memory_audit_images = []
subdir('platform')
if get_option('bootloader')
  if platform not in ['stm32h5', 'stm32h7'] or not get_option('examples')
    error('Bootloader requires STM32 and examples=true')
  endif
  subdir('boot' / board)
endif
# Stable dependency names for applications using DaveOS as a Meson subproject.
meson.override_dependency('daveos-core', core_dep)
meson.override_dependency('daveos-console', console_dep)
if get_option('networking')
  meson.override_dependency('daveos-network', network_dep)
endif
if platform in ['host', 'fake']
  meson.override_dependency('daveos-fake', fake_dep)
  if platform == 'host'
    meson.override_dependency('daveos-host', host_dep)
  endif
elif platform == 'stm32h5'
  meson.override_dependency('daveos-stm32h5', stm32h5_dep)
else
  meson.override_dependency('daveos-stm32h7', stm32h7_dep)
endif
if get_option('examples')
  subdir('examples')
endif
if get_option('examples') and firmware_images.length() > 0
  subdir('tools/programming')
endif
if memory_audit_images.length() > 0
  subdir('tools/memory')
endif
if platform in ['stm32h5', 'stm32h7']
  subdir('tests/arm')
elif get_option('tests')
  subdir('tests')
endif
