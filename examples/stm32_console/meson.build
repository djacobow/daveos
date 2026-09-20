# Meson selects optional members; Console supplies registration and shutdown.
component_deps = [stm32_console_dep, stm32_nucleo_dep]
application_linker = board_linker_script
application_link_args = []
if get_option('bootloader')
  component_deps = [stm32_console_dep, stm32_nucleo_sources_dep, boot_layout_dep]
  application_linker = boot_layout[2]
  application_link_args = ['-T' + boot_layout_directory / 'slot_a.ld']
endif
component_includes = []
component_members = []
component_consoles = []
component_modules = []
component_stop = []
component_early = []
component_failure = []
if board == 'h563'
  component_deps += util_dep
  component_deps += declare_dependency(sources: files('../../platform/stm32h5/iwdg_handler.S'))
  component_includes += '#include "health.hpp"'
  component_members += '    Health health{platform};'
  component_modules += ', health'
  component_early += '      health.attach_progress(scheduler);'
  if get_option('bootloader')
    component_early += '      health.confirmation(&boot, [](void* p) { return static_cast<Boot*>(p)->Confirm(); });'
  endif
  component_early += '      auto result = health.early_start();'
  component_early += '      if (result != core::Status::ok) { health.initialization_failed(result); }'
  component_failure += '      health.initialization_failed(status, module);'
endif
if get_option('bootloader')
  component_includes += '#include "boot.hpp"'
  component_members += '    Boot boot{platform};'
  component_modules += ', boot'
endif

if get_option('otp_backend') == 'flash-emulator'
  component_deps += otp_dep
  component_includes += '#include "otp.hpp"'
  component_members += '    Otp otp{platform};'
  component_modules += ', otp.module'
endif

if get_option('otp_backend') == 'h563'
  component_deps += otp_dep
  component_includes += '#include "otp_hardware.hpp"'
  component_members += '    HardwareOtp otp{platform, ' + (get_option('otp_programming') ? 'true' : 'false') + '};'
  component_modules += ', otp.module, otp'
endif

if get_option('networking')
  component_includes += '#include "platform/stm32/console/network.hpp"'
  component_members += '    stm32::Network<Event> network{platform};'
  component_modules += ', network.module'
  component_stop += '      network.stop();'
  if get_option('bootloader')
    component_deps += network_update_dep
    component_includes += '#include "ota.hpp"'
    component_members += '    Ota ota{platform, network.service};'
    component_modules += ', ota.module'
    component_stop = ['      ota.stop();'] + component_stop
  endif
  if get_option('tcp_console')
    component_includes += '#include "console/tcp.hpp"'
    component_members += '    daveos::console::TcpConsole<Event, Platform> tcp{platform, network.service};'
    component_consoles += 'tcp'
  endif
endif
if get_option('uart_console')
  component_deps += stm32_uart_console_dep
  component_includes += '#include "platform/stm32/console/uart.hpp"'
  component_members += '    stm32::UartConsole<Event> uart{platform};'
  component_consoles += 'uart'
endif
if get_option('usb_console')
  component_includes += '#include "platform/stm32/console/usb/usb.hpp"'
  component_members += '    stm32::UsbConsole<Event> usb{platform};'
  component_consoles += 'usb'
endif
composition = configuration_data()
composition.set('INCLUDES', '\n'.join(component_includes))
composition.set('MEMBERS', '\n'.join(component_members))
composition.set('CONSOLES', ', '.join(component_consoles))
composition.set('MODULES', ''.join(component_modules))
composition.set('STOP', '\n'.join(component_stop))
composition.set('EARLY', '\n'.join(component_early))
composition.set('FAILURE', '\n'.join(component_failure))
configure_file(input: 'composition.hpp.in', output: 'composition.hpp', configuration: composition)

firmware = executable('stm32-console', 'appmain.cpp',
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
  factory_image = custom_target('factory-image',
    input: [boot_binary, application_binary, boot_layout[4], version_header[1]],
    output: 'factory.hex',
    command: [python, files('../../tools/factory.py'), '--bootloader', '@INPUT0@',
      '--application', '@INPUT1@', '--layout', '@INPUT2@', '--version', '@INPUT3@',
      '--output', '@OUTPUT@'], build_by_default: true,
    depends: boot_size_check)
  firmware_images += [factory_image]
else
  firmware_images += [firmware]
endif
