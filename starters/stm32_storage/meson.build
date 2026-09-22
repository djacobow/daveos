project('my-storage-app', ['c', 'cpp'],
  default_options: ['cpp_std=c++20', 'cpp_eh=none', 'warning_level=3', 'buildtype=debug'])

# A second Nucleo application built only from DaveOS libraries: USART3
# console, board commands, the Nucleo SD card (stm32::SdCard with the reusable
# storage::sd::Session) and read-only FatFs commands. Nothing comes from
# DaveOS's examples/ directory.
board = get_option('board')
nucleo = dependency('daveos-nucleo', fallback: ['daveos', 'stm32_nucleo_dep'],
  default_options: ['platform=stm32', 'board=' + board, 'stm32_support=true',
                   'examples=false', 'tests=false', 'features=uart,sd,fatfs'])
uart = dependency('daveos-stm32-uart', fallback: ['daveos', 'stm32_uart_console_dep'])
storage = dependency('daveos-storage', fallback: ['daveos', 'storage_dep'])
hal = dependency('daveos-hal', fallback: ['daveos', 'hal_dep'])
daveos = subproject('daveos')
if daveos.get_variable('board') != board
  error('Board selections differ: configure both -Dboard=' + board + ' and -Ddaveos:board=' + board)
endif
firmware = executable('my-storage', 'appmain.cpp', name_suffix: 'elf',
  dependencies: [nucleo, uart, storage, hal], include_directories: include_directories('.'),
  cpp_args: ['-fno-freestanding', '-fno-rtti', '-D_GLIBCXX_NO_ASSERTIONS'],
  link_args: ['-Wl,--print-memory-usage',
             '-Wl,-Map=' + meson.current_build_dir() / 'my-storage.map'],
  link_depends: daveos.get_variable('stm32_nucleo_linker_script'))
objcopy = find_program('arm-none-eabi-objcopy')
foreach format : [['hex', 'ihex'], ['bin', 'binary']]
  custom_target('my-storage-' + format[0], input: firmware,
    output: 'my-storage.' + format[0],
    command: [objcopy, '-O', format[1], '@INPUT@', '@OUTPUT@'], build_by_default: true)
endforeach

# H755's support dependency supplies its sleeping M4 image too. Program both.
images = daveos.get_variable('stm32_nucleo_aux_images') + [firmware]
python = import('python').find_installation('python3')
foreach backend : ['cubeprogrammer', 'openocd', 'plan']
  name = backend == 'cubeprogrammer' ? 'flash' : 'flash-' + backend
  run_target(name, command: [python, daveos.get_variable('flash_script'),
    '--backend', backend, '--family', board == 'h563' ? 'stm32h5' : 'stm32h7',
    '--serial', get_option('probe_serial'), '--openocd', get_option('openocd'),
    '--cubeprogrammer', get_option('cubeprogrammer')] + images)
endforeach
