logging_source = get_option('logging') ? 'logging.cpp' : 'disabled.cpp'
exe = executable('test-logging', logging_source, dependencies: test_support_dep)
test('logging', exe, suite: 'unit', timeout: 20)

# Verify the public printf annotation, including failure for mismatched types.
cpp = meson.get_compiler('cpp')
if cpp.get_id() in ['gcc', 'clang', 'apple-clang']
  format_check = '''
    #include "core/schedule/module.hpp"
    using Event = std::variant<std::monostate>;
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

if get_option('logging')
  platform_check = '''
    // uses_platform takes the logger's concrete platform type directly.
    #include "core/logging/logger.hpp"
    namespace core = daveos::core;
    struct TestPlatform {};
    struct Other {};
    void check(core::Logger<32, 128, 0, TestPlatform>& logger, @0@& platform) {
      logger.uses_platform(platform);
    }
  '''
  foreach platform_type, expected : {'TestPlatform': true, 'Other': false}
    if cpp.compiles(platform_check.format(platform_type), dependencies: core_dep,
        name: 'logger platform type ' + platform_type) != expected
      error('Logger platform type validation did not behave as expected')
    endif
  endforeach
endif
