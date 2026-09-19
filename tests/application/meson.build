exe = executable('test-application', 'application.cpp', dependencies: test_support_dep)
test('application', exe, suite: 'unit')

cpp = meson.get_compiler('cpp')
foreach interval : ['std::chrono::microseconds{0}', 'std::chrono::microseconds{-1}',
                    'std::chrono::nanoseconds{1}', 'std::chrono::hours{INT64_MAX}',
                    'std::chrono::duration<double>{1}', '10']
  code = '''#include "core/schedule/module.hpp"
    struct Worker {
      void Poll() {}
      static constexpr auto tasks() {
        return std::array{DAVEOS_PERIODIC(Worker, Poll, ''' + interval + ''')};
      }
    };
    int main() { constexpr auto tasks = Worker::tasks(); (void)tasks; }
  '''
  if cpp.compiles(code, dependencies: core_dep, name: 'reject periodic interval ' + interval)
    error('Invalid periodic interval unexpectedly compiled')
  endif
endforeach

factory_check = '''
#include "core/schedule/application.hpp"
#include "core/logging/logger.hpp"
#include "platform/fake/platform.h"
namespace core = daveos::core;
struct Worker : core::Module<Worker> {
  static constexpr const char* name() { return "worker"; }
};
struct BadLogger { int counters() { return 0; } };
int main() {
  daveos::platform::fake::Platform platform;
  Worker worker;
  auto modules = core::ModuleList{&worker};
  core::CommandSource source;
  auto sources = core::CommandSourceList{source};
  BadLogger bad;
  @0@
}
'''
foreach item : [
  ['auto app = core::make_application(platform, modules);', true],
  ['auto app = core::make_application<core::NoEvent, core::Capacities{.events = 2}>(platform, modules);', true],
  ['auto app = core::make_application(platform, modules, bad);', false],
  ['auto app = core::make_application(platform, modules, bad, sources);', false],
  ['auto app = core::make_application<core::NoEvent, core::Capacities{.line = 0}>(platform, modules, sources);', false],
  ['auto app = core::make_application<core::NoEvent, core::Capacities{.arguments = 1}>(platform, modules, sources);', false],
]
  if cpp.compiles(factory_check.format(item[0]), dependencies: core_dep,
      name: 'application factory ' + item[0]) != item[1]
    error('Application factory validation did not behave as expected')
  endif
endforeach
