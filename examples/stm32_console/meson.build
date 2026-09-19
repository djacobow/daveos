# Components own behavior; this file selects their sources and static wiring.
component_sources = files('appmain.cpp')
component_includes = []
component_members = []
component_modules = []
component_subscribers = []
component_inputs = []
component_statistics = []
component_stop = []

if get_option('networking')
  component_sources += files('network.cpp')
  component_includes += '#include "network.h"'
  component_members += '    Network network{platform};'
  component_modules += '&network.module'
  component_stop = ['      network.stop();'] + component_stop
  if get_option('tcp_console')
    component_includes += '#include "tcp_console.hpp"'
    component_members += '    TcpConsole<Event, Platform> tcp{platform, network.service};'
    component_modules += '&tcp'
    component_subscribers += 'tcp.subscriber()'
    component_inputs += 'tcp.command_source()'
    component_stop = ['      tcp.stop();'] + component_stop
  endif
endif

if get_option('uart_console')
  component_sources += files('uart.cpp')
  component_includes += '#include "uart.h"'
  component_members += '    UartConsole uart{platform};'
  component_modules += '&uart'
  component_subscribers += 'uart.subscriber()'
  component_inputs += 'uart.command_source()'
  component_statistics += '      uart.log_statistics(scheduler);'
  component_stop = ['      uart.stop();'] + component_stop
endif

if get_option('usb_console')
  subdir('usb')
  component_includes += '#include "usb/component.h"'
  component_members += '    Usb usb{platform};'
  component_modules += '&usb.module'
  component_subscribers += 'usb.module.subscriber()'
  component_inputs += 'usb.module.command_source()'
  component_statistics += '      usb.log_statistics(scheduler);'
  component_stop = ['      usb.stop();'] + component_stop
endif

composition = configuration_data()
composition.set('INCLUDES', '\n'.join(component_includes))
composition.set('MEMBERS', '\n'.join(component_members))
composition.set('MODULES', ',\n        '.join(component_modules))
composition.set('SUBSCRIBERS', ',\n        '.join(component_subscribers))
composition.set('SOURCES', ',\n        '.join(component_inputs))
composition.set('STATISTICS', '\n'.join(component_statistics))
composition.set('STOP', '\n'.join(component_stop))
configure_file(input: 'composition.hpp.in', output: 'composition.hpp',
  configuration: composition)

# Both the source and generated include directories travel with this dependency.
stm32_console_dep = declare_dependency(sources: component_sources,
  include_directories: include_directories('.'), dependencies: console_dep)

# Each board contributes startup, HAL configuration, pin/peripheral glue, and
# a linker script. The application target and optional components are shared.
objcopy = find_program('objcopy')
subdir('boards' / board)
firmware = executable('stm32-console', board_sources,
  dependencies: firmware_deps, name_suffix: 'elf',
  # libstdc++ assertion reporting pulls in allocating stdio on this toolchain.
  override_options: ['b_ndebug=true'],
  include_directories: board_includes,
  c_args: board_args,
  # ST's device headers include math.h; allow it for the application, while
  # the core compile check remains freestanding.
  cpp_args: board_args + ['-D_GLIBCXX_NO_ASSERTIONS', '-fno-freestanding',
                         '-fno-rtti', '-fstack-usage'],
  link_args: ['-T' + board_linker_path, '-Wl,--gc-sections',
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
