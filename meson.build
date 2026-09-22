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

# Feature selection. One entry per feature: 'stm32' (needs an STM32 build),
# 'boards' (supported boards), 'requires' (other features), 'bootloader'
# (needs -Dbootloader=true). Order here is the resolved order.
is_stm32 = platform in ['stm32h5', 'stm32h7']
feature_order = ['uart', 'usb', 'net', 'tcp', 'health', 'sd', 'sd-dma', 'fatfs',
                 'i2c-adc', 'ota', 'otp-emulator', 'otp-h563']
feature_rules = {
  'uart': {'stm32': true},
  'usb': {'stm32': true},
  'net': {},
  'tcp': {'requires': ['net']},
  'health': {'stm32': true},
  'sd': {'stm32': true},
  'sd-dma': {'stm32': true, 'requires': ['sd']},
  # On STM32 the example mounts the SD card; host builds use file images.
  'fatfs': {'requires': is_stm32 ? ['sd'] : []},
  'i2c-adc': {'stm32': true, 'boards': ['h563']},
  'ota': {'stm32': true, 'bootloader': true},
  'otp-emulator': {'stm32': true, 'boards': ['h563'], 'bootloader': true},
  'otp-h563': {'stm32': true, 'boards': ['h563'], 'bootloader': true},
}
requested_features = []
foreach name : get_option('features')
  if name == 'default'
    requested_features += is_stm32 ? ['uart', 'usb', 'health'] : []
  else
    requested_features += name
  endif
endforeach
features = []
foreach name : feature_order
  if name in requested_features
    features += name
  endif
endforeach
foreach name : features
  rule = feature_rules[name]
  if rule.get('stm32', false) and not is_stm32
    error('Feature ' + name + ' requires an STM32 build (-Dplatform=stm32)')
  endif
  supported_boards = rule.get('boards', [])
  if is_stm32 and supported_boards.length() > 0 and board not in supported_boards
    error('Feature ' + name + ' is not supported on ' + board
      + ' (supported: ' + ', '.join(supported_boards) + ')')
  endif
  foreach required : rule.get('requires', [])
    if required not in features
      error('Feature ' + name + ' requires ' + required)
    endif
  endforeach
  if rule.get('bootloader', false) and not get_option('bootloader')
    error('Feature ' + name + ' requires -Dbootloader=true')
  endif
endforeach
if 'ota' in features and 'net' not in features and 'fatfs' not in features
  error('Feature ota needs an upload source: add net (TCP) or fatfs (SD file)')
endif
if 'otp-emulator' in features and 'otp-h563' in features
  error('Features otp-emulator and otp-h563 are mutually exclusive')
endif
if get_option('otp_programming') and 'otp-h563' not in features
  error('otp_programming requires the otp-h563 feature')
endif
message('Features: ' + (features.length() > 0 ? ', '.join(features) : 'none'))

subdir('tools')
subdir('core')
subdir('hal')
subdir('drivers')
if 'fatfs' in features
  subdir('storage')
endif
subdir('util')
subdir('boot')
subdir('update')
subdir('otp')
subdir('console')
if 'net' in features
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
if 'net' in features
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
