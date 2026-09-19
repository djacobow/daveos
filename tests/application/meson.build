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
