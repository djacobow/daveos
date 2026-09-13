exe = executable('test-logging', '../logging.cc', dependencies: [fake_dep, catch_dep])
test('logging', exe, suite: 'unit', timeout: 20)

# Verify the public printf annotation, including failure for mismatched types.
cpp = meson.get_compiler('cpp')
if cpp.get_id() in ['gcc', 'clang', 'apple-clang']
  format_check = '''
    #include "daveos/core/module.h"
    enum class Event { sample };
    template <typename E>
    struct Example : daveos::core::Module<Example<E>, E> {
      void check() {
        this->scheduler().log(daveos::core::Level::info, "%s %d", @0@);
        D_("%s %d", @0@);
        I_("%s %d", @0@);
        W_("%s %d", @0@);
        E_("%s %d", @0@);
        F_("%s %d", @0@);
        I_("no arguments");
      }
    };
    template struct Example<Event>;
  '''
  foreach argument, expected : {'"value", 42': true, '42, "value"': false}
    accepted = cpp.compiles(format_check.format(argument),
      dependencies: core_dep, args: ['-Wformat', '-Werror=format'],
      name: 'log format arguments ' + argument)
    if accepted != expected
      error('SchedulerInterface::log format checking did not behave as expected')
    endif
  endforeach
endif
