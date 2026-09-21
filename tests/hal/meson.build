hal_test = executable('test-hal', 'hal.cpp', dependencies: [test_support_dep, hal_dep])
test('hal', hal_test, suite: 'unit')
cpp = meson.get_compiler('cpp')
lookup_check = '''
#include "hal/spi/action.hpp"
#include "hal/registry.hpp"
enum class Alias { one, two, unknown };
namespace hal = daveos::hal;
auto check() {
  hal::Registry<hal::spi::Action, @0@> registry{std::array{hal::spi::Device{}, hal::spi::Device{}}};
  return registry.device<@1@>();
}
'''
foreach variant : [
  ['Alias::one, Alias::two', 'Alias::one', true],
  ['Alias::one, Alias::one', 'Alias::one', false],
  ['Alias::one, Alias::two', 'Alias::unknown', false],
]
  if cpp.compiles(lookup_check.format(variant[0], variant[1]), dependencies: hal_dep,
                 name: 'HAL device aliases ' + variant[0] + ' lookup ' + variant[1]) != variant[2]
    error('HAL compile-time alias validation failed')
  endif
endforeach
