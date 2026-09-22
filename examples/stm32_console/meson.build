# Meson selects optional members; Console supplies registration and shutdown.
component_deps = [stm32_console_dep, stm32_nucleo_dep]
application_linker = board_linker_script
application_link_args = []
if get_option('bootloader')
  component_deps = [stm32_console_dep, stm32_nucleo_sources_dep, boot_layout_dep]
  application_linker = boot_layout[2]
  application_link_args = ['-T' + boot_layout_directory / 'slot_a.ld']
endif
# One entry per component, in declaration order (later members may refer to
# earlier ones). Components come from the top-level feature list, plus boot
# for -Dbootloader=true and ota-net when both ota and net are selected.
# Fields: deps (dependency variable names, resolved only when selected),
# sources, include, members, modules, consoles, and one-line calls for the
# composition's early/failure/stop hooks. Behavior belongs in the C++ types.
sd_dma = 'sd-dma' in features
file_ota = 'ota' in features and 'fatfs' in features
components = {
  'i2c-bus': {
    'deps': ['drivers_dep'], 'sources': ['irqs/i2c.cpp'],
    'include': ['i2c_probe.hpp'],
    'members': ['I2cBus i2c1{platform};', 'I2cProbe i2c{i2c1};'],
    'modules': ['i2c'],
  },
  'i2c-adc': {
    'include': ['adc_probe.hpp'],
    'members': ['AdcProbe adc{platform, i2c1};'],
    'modules': ['adc'],
  },
  'ssd1306': {
    'include': ['display.hpp'],
    'members': ['Display display{i2c1};'],
    'modules': ['display'],
  },
  'sd': {
    'deps': ['hal_dep'],
    'sources': ['irqs/sd.cpp'] + (sd_dma ? ['irqs/sd_dma_' + board + '.cpp'] : []),
    'include': ['sd_probe.hpp'],
    'members': ['SdProbe sd{platform, ' + (sd_dma ? 'true' : 'false') + '};'],
    'modules': ['sd'],
  },
  'fatfs': {
    'deps': ['storage_dep'], 'include': ['storage/module.hpp'],
    'members': ['daveos::storage::Module<Event> filesystem{sd.block_device()};'],
    'modules': ['filesystem'],
  },
  'health': {
    'deps': ['util_dep'],
    'sources': board == 'h563' ? ['../../platform/stm32h5/iwdg_handler.S'] : [],
    'include': ['platform/stm32/console/health.hpp', 'util/version_stamp.h'],
    'members': ['stm32::Health<Event> health{platform, daveos::build::kApplicationVersion};'],
    'modules': ['health'],
    'early': (get_option('bootloader') ? ['boot.attach(health);'] : [])
             + ['health.early(scheduler);'],
    'failure': ['health.initialization_failed(status, module);'],
  },
  'boot': {
    'include': ['boot.hpp'], 'members': ['Boot boot{platform};'],
    'modules': ['boot'],
  },
  'otp-emulator': {
    'deps': ['otp_dep'], 'include': ['otp.hpp'], 'members': ['Otp otp{platform};'],
    'modules': ['otp.module'],
  },
  'otp-h563': {
    'deps': ['otp_dep'], 'include': ['otp_hardware.hpp'],
    'members': ['HardwareOtp otp{platform, '
                + (get_option('otp_programming') ? 'true' : 'false') + '};'],
    'modules': ['otp.module', 'otp'],
  },
  'ota': {
    'deps': ['update_dep'], 'include': ['ota.hpp'],
    'members': [file_ota ? 'Ota<true> ota{platform, filesystem.read_file()};'
                         : 'Ota<> ota{platform};'],
    'modules': ['ota.module'],
  },
  'net': {
    'include': ['platform/stm32/console/network.hpp'],
    'members': ['stm32::Network<Event> network{platform};'],
    'modules': ['network.module'], 'stop': ['network.stop();'],
  },
  'ota-net': {
    'deps': ['network_update_dep'], 'include': ['ota_network.hpp'],
    'members': ['OtaNetwork ota_network{platform, network.service, ota};'],
    'stop': ['ota_network.stop();'],
  },
  'file-transfer': {
    'deps': ['storage_dep', 'network_dep'],
    'include': ['net/file_transfer.hpp'],
    'members': ['daveos::net::FileTransferModule<Platform, Event> files{platform, network.service, filesystem.file_access()};'],
    'modules': ['files'],
  },
  'tcp': {
    'include': ['console/tcp.hpp'],
    'members': ['daveos::console::TcpConsole<Event, Platform> tcp{platform, network.service};'],
    'consoles': ['tcp'],
  },
  'uart': {
    'deps': ['stm32_uart_console_dep'], 'include': ['platform/stm32/console/uart.hpp'],
    'members': ['stm32::UartConsole<Event> uart{platform};'], 'consoles': ['uart'],
  },
  'usb': {
    'include': ['platform/stm32/console/usb/usb.hpp'],
    'members': ['stm32::UsbConsole<Event> usb{platform};'], 'consoles': ['usb'],
  },
}
component_order = ['i2c-bus', 'i2c-adc', 'ssd1306', 'sd', 'fatfs', 'health', 'boot', 'otp-emulator',
                   'otp-h563', 'ota', 'net', 'ota-net', 'file-transfer', 'tcp', 'uart', 'usb']
selected = features + (get_option('bootloader') ? ['boot'] : [])
selected += ('i2c-adc' in features or 'ssd1306' in features) ? ['i2c-bus'] : []
selected += 'ota' in features and 'net' in features ? ['ota-net'] : []

component_includes = []
component_members = []
component_consoles = []
component_modules = []
component_stop = []
component_early = []
component_failure = []
component_sources = []
foreach name : component_order
  if name not in selected
    continue
  endif
  entry = components[name]
  foreach dep : entry.get('deps', [])
    component_deps += get_variable(dep)
  endforeach
  component_sources += files(entry.get('sources', []))
  foreach header : entry.get('include', [])
    component_includes += '#include "' + header + '"'
  endforeach
  foreach member : entry.get('members', [])
    component_members += '    ' + member
  endforeach
  foreach module : entry.get('modules', [])
    component_modules += ', ' + module
  endforeach
  component_consoles += entry.get('consoles', [])
  foreach call : entry.get('early', [])
    component_early += '      ' + call
  endforeach
  foreach call : entry.get('failure', [])
    component_failure += '      ' + call
  endforeach
  # Stop in reverse declaration order.
  foreach call : entry.get('stop', [])
    component_stop = ['      ' + call] + component_stop
  endforeach
endforeach
composition = configuration_data()
composition.set('INCLUDES', '\n'.join(component_includes))
composition.set('MEMBERS', '\n'.join(component_members))
composition.set('CONSOLES', ', '.join(component_consoles))
composition.set('MODULES', ''.join(component_modules))
composition.set('STOP', '\n'.join(component_stop))
composition.set('EARLY', '\n'.join(component_early))
composition.set('FAILURE', '\n'.join(component_failure))
configure_file(input: 'composition.hpp.in', output: 'composition.hpp', configuration: composition)

# Record what this firmware contains beside it, for reproducing hardware runs.
build_config = configuration_data()
build_config.set('BOARD', board)
build_config.set('BOOTLOADER', get_option('bootloader') ? 'true' : 'false')
build_config.set('LOGGING', get_option('logging') ? 'true' : 'false')
build_config.set('FEATURES',
  features.length() > 0 ? '"' + '", "'.join(features) + '"' : '')
configure_file(input: 'build-config.json.in', output: 'build-config.json',
  configuration: build_config)

firmware = executable('stm32-console', ['appmain.cpp'] + component_sources,
  dependencies: component_deps, name_suffix: 'elf',
  # libstdc++ assertion reporting pulls in allocating stdio on this toolchain.
  override_options: ['b_ndebug=true'],
  include_directories: include_directories('.'),
  c_args: board_args,
  # ST's device headers include math.h; allow it for the application, while
  # the core compile check uses the same header mode for C++20 <chrono>.
  cpp_args: board_args + ['-D_GLIBCXX_NO_ASSERTIONS', '-fno-freestanding',
                         '-fno-rtti', '-fstack-usage'],
  link_args: application_link_args + [
             '-Wl,--print-memory-usage',
             '-Wl,-Map=' + meson.current_build_dir() / 'stm32-console.map'],
  link_depends: application_linker)
foreach output_format : [['hex', 'ihex'], ['bin', 'binary']]
  converted = custom_target('stm32-console-' + output_format[0],
    input: firmware, output: 'stm32-console.' + output_format[0],
    command: [objcopy, '-O', output_format[1], '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
  if output_format[0] == 'bin'
    application_binary = converted
  endif
endforeach
if get_option('bootloader')
  slot_b_dependencies = []
  foreach dep : component_deps
    slot_b_dependencies += dep.partial_dependency(links: true, link_args: true)
  endforeach
  slot_b = executable('stm32-console-b',
    objects: firmware.extract_all_objects(recursive: false),
    dependencies: slot_b_dependencies, name_suffix: 'elf',
    link_args: ['-T' + boot_layout_directory / 'slot_b.ld',
      '-Wl,--print-memory-usage',
      '-Wl,-Map=' + meson.current_build_dir() / 'stm32-console-b.map'],
    link_depends: boot_layout[3])
  slot_b_binary = custom_target('stm32-console-b-bin', input: slot_b,
    output: 'stm32-console-b.bin', command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
  custom_target('application-package',
    input: [application_binary, slot_b_binary, boot_layout[4], version_header[1]],
    output: 'application.ota',
    command: [python, files('../../tools/package.py'), '--a', '@INPUT0@', '--b', '@INPUT1@',
      '--layout', '@INPUT2@', '--version', '@INPUT3@', '--output', '@OUTPUT@'],
    build_by_default: true)
  factory_inputs = [boot_binary, application_binary, boot_layout[4], version_header[1]]
  factory_args = []
  if board == 'h755'
    factory_inputs += m4_binary
    factory_args = ['--m4', '@INPUT4@']
  endif
  factory_image = custom_target('factory-image',
    input: factory_inputs,
    output: 'factory.hex',
    command: [python, files('../../tools/factory.py'), '--bootloader', '@INPUT0@',
      '--application', '@INPUT1@', '--layout', '@INPUT2@', '--version', '@INPUT3@',
      '--output', '@OUTPUT@'] + factory_args, build_by_default: true,
    depends: boot_size_check)
  firmware_images += [factory_image]
else
  firmware_images += [firmware]
endif

memory_audit_images += [firmware]
if get_option('bootloader')
  memory_audit_images += [slot_b]
endif
