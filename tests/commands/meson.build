exe = executable('test-commands', 'commands.cpp', dependencies: test_support_dep)
test('commands', exe, suite: 'unit', timeout: 20)

# Positive control prevents an unrelated compile failure from hiding invalid
# metadata checks. Each variant instantiates the actual registration boundary.
cpp = meson.get_compiler('cpp')
registration_check = '''
  #include "core/command/command.hpp"
  enum class Event {};
  using namespace daveos::core;
  struct Example : Module<Example, Event> {
    static constexpr const char* name() { return "example"; }
    Status Run(CommandArguments) { return Status::ok; }
    static constexpr auto commands() {
      return std::array{@0@};
    }
  };
  using List = ModuleList<Example>;
  static_assert(sizeof(CommandDispatcher<Event, List>) > 0);
'''
foreach descriptor, expected : {
  'DAVEOS_COMMAND(Example, "run", Run, "Run command")': true,
  'DAVEOS_COMMAND(Example, "run", Run, "Run"), DAVEOS_COMMAND(Example, "RUN", Run, "Duplicate")': false,
  'DAVEOS_COMMAND(Example, "HELP", Run, "Reserved")': false,
  'DAVEOS_COMMAND(Example, "bad name", Run, "Invalid")': false,
  'DAVEOS_COMMAND(Example, "", Run, "Empty")': false,
  'CommandDescriptor<Example>{"run", "Run", nullptr, "Run"}': false,
  'DAVEOS_COMMAND(Example, "run", Run, "")': false,
}
  if cpp.compiles(registration_check.format(descriptor), dependencies: core_dep,
      name: 'command metadata ' + descriptor) != expected
    error('Command metadata validation did not behave as expected')
  endif
endforeach
names_check = '''
  #include "core/command/command.hpp"
  enum class Event {};
  using namespace daveos::core;
  template <int N> struct Example : Module<Example<N>, Event> {
    static constexpr const char* name() { return N == 0 ? "one" : @0@; }
    static constexpr const char* command_prefix() { return N == 0 ? "one" : @1@; }
    Status Run(CommandArguments) { return Status::ok; }
    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Example, "run", Run, "Run")};
    }
  };
  using List = ModuleList<Example<0>, Example<1>>;
  static_assert(sizeof(List) > 0);
  static_assert(sizeof(CommandDispatcher<Event, List>) > 0);
'''
foreach variant : [
  ['"two"', '"two"', true],
  ['"ONE"', '"two"', false],
  ['""', '"two"', false],
  ['"two"', '"ONE"', false],
  ['"two"', '"help"', false],
  ['"two"', '"bad prefix"', false],
]
  if cpp.compiles(names_check.format(variant[0], variant[1]), dependencies: core_dep,
      name: 'module name/prefix ' + variant[0] + '/' + variant[1]) != variant[2]
    error('Module name/prefix validation did not behave as expected')
  endif
endforeach

# Exercise the real API boundaries, including a matching positive control.
event_check = '''
  #include "core/command/command.hpp"
  #include "core/schedule/scheduler.hpp"
  #include "platform/fake/platform.h"
  using namespace daveos::core;
  enum class Event {};
  enum class Other {};
  struct Example : Module<Example, @0@> {
    static constexpr const char* name() { return "example"; }
    void Task() {}
    static constexpr auto tasks() {
      return std::array{TaskDescriptor<Example>{"task", &Example::Task}};
    }
  };
  static_assert(!ModuleFor<int, Event>);
  void check(SchedulerInterface<Event>& interface, Example& module,
             daveos::platform::fake::Platform& platform) {
    @1@;
  }
'''
foreach operation : [
    'interface.schedule(module, &Example::Task, 1)',
    'interface.cancel(module, &Example::Task)',
    'auto scheduler = make_scheduler<Event>(platform, ModuleList{&module})',
    'CommandDispatcher<Event, ModuleList<Example>> dispatcher(ModuleList{&module}, interface)']
  foreach event_type, expected : {'Event': true, 'Other': false}
    if cpp.compiles(event_check.format(event_type, operation), dependencies: core_dep,
        name: 'module event ' + event_type + ': ' + operation) != expected
      error('Module event compatibility did not behave as expected')
    endif
  endforeach
endforeach
