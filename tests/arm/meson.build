# Match the firmware's C++ library mode: GCC 13's <chrono> requires hosted
# headers. This still cross-compiles against newlib without host/OS APIs;
# allowing these headers does not imply allocating or using hosted facilities.
static_library('daveos-arm-core-check', 'arm_compile.cpp', dependencies: core_dep,
  cpp_args: ['-fno-freestanding'])
