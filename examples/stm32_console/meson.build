# Meson selects optional members; Console supplies registration and shutdown.
component_deps = [stm32_console_dep, stm32_nucleo_dep]
component_includes = []
component_members = []
component_consoles = []
component_modules = []
component_stop = []

if get_option('networking')
  component_includes += '#include "platform/stm32/console/network.hpp"'
  component_members += '    stm32::Network<Event> network{platform};'
  component_modules += ', network.module'
  component_stop += '      network.stop();'
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
  link_args: [
             '-Wl,--print-memory-usage',
             '-Wl,-Map=' + meson.current_build_dir() / 'stm32-console.map'],
  link_depends: board_linker_script)
foreach output_format : [['hex', 'ihex'], ['bin', 'binary']]
  custom_target('stm32-console-' + output_format[0],
    input: firmware, output: 'stm32-console.' + output_format[0],
    command: [objcopy, '-O', output_format[1], '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
endforeach
firmware_images += [firmware]
