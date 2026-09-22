exe = executable('test-usability', 'usability.cpp', dependencies: test_support_dep)
test('usability', exe, suite: 'unit')

cpp = meson.get_compiler('cpp')
# Public API errors should be diagnosed while compiling the caller.
foreach invalid : [
  '''#include "core/schedule/module.hpp"
     using E = std::variant<std::monostate>;
     struct Bad : daveos::core::Module<Bad,E> {
       void Work() {}
       void Use() { (void)schedule<&Bad::Work>(std::chrono::microseconds{1}); }
     };
     int main() { Bad b; b.Use(); }''',
  '''#include "core/schedule/module.hpp"
     using E = std::variant<std::monostate>;
     struct Bad : daveos::core::Module<Bad,E> { void init(daveos::core::InitStage) {} };
     int main() { Bad b; (void)b.initialize(daveos::core::InitStage::stage1); }''',
  '''#include "core/schedule/module.hpp"
     using E = std::variant<std::monostate>;
     int main() { daveos::core::SchedulerInterface<E> s;
       (void)s.timer(std::chrono::duration<double>{1}, [] {}); }''',
  '''#include "core/platform/timer_callback.hpp"
     struct Bad { int Fire() { return 1; } };
     int main() { Bad b; (void)daveos::core::TimerCallback::bind<&Bad::Fire>(b); }'''
]
  if cpp.compiles(invalid, dependencies: core_dep, name: 'reject invalid convenience API use')
    error('Invalid convenience API use unexpectedly compiled')
  endif
endforeach
