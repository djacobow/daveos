stm32_ethernet_dep = declare_dependency(
  sources: files('driver.cpp', '../lan8742/lan8742.c'),
  include_directories: include_directories('../lan8742'),
  compile_args: ['-DHAL_ETH_MODULE_ENABLED'],
  dependencies: network_dep)
