stm32_ethernet_dep = declare_dependency(
  sources: files('driver.cpp', '../../../third_party/lan8742/lan8742.c'),
  include_directories: include_directories('../../../third_party/lan8742'),
  compile_args: ['-DHAL_ETH_MODULE_ENABLED'],
  dependencies: network_dep)
