# Board startup/peripheral support is a source dependency: it compiles in the
# consumer, whose appmain.h/appmain() supply the application entry point.
objcopy = find_program('objcopy')
subdir(board)
stm32_nucleo_dep = declare_dependency(sources: board_sources,
  include_directories: board_includes, dependencies: firmware_deps,
  compile_args: board_args + cpu_flags,
  link_args: cpu_flags + ['--specs=nosys.specs', '-T' + board_linker_path,
                         '-Wl,--gc-sections'])
stm32_nucleo_linker_script = board_linker_script
stm32_nucleo_aux_images = firmware_images
meson.override_dependency('daveos-nucleo', stm32_nucleo_dep)
meson.override_dependency('daveos-stm32-console', stm32_console_dep)
meson.override_dependency('daveos-stm32-uart', stm32_uart_console_dep)
if get_option('usb_console')
  meson.override_dependency('daveos-stm32-usb', stm32_usb_console_dep)
endif
