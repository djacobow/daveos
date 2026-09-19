exe = executable('test-commands', ['commands.cpp', 'typed.cpp', 'argument_matrix.cpp', 'board.cpp', 'choices.cpp'], dependencies: test_support_dep,
  include_directories: include_directories('.'))
test('commands', exe, suite: 'unit', timeout: 20)

# Positive control prevents an unrelated compile failure from hiding invalid
# metadata checks. Each variant instantiates the actual registration boundary.
cpp = meson.get_compiler('cpp')
registration_check = '''
  #include "core/command/command.hpp"
  enum class Event {};
  namespace core = daveos::core;
  struct Example : core::Module<Example, Event> {
    static constexpr const char* name() { return "example"; }
    core::Status Run(core::CommandArguments) { return core::Status::ok; }
    static constexpr auto commands() {
      return std::array{@0@};
    }
  };
  using List = core::ModuleList<Example>;
  static_assert(sizeof(core::CommandDispatcher<Event, List>) > 0);
'''
foreach descriptor, expected : {
  'DAVEOS_COMMAND(Example, Run, "run", "Run command")': true,
  'DAVEOS_COMMAND(Example, Run, "run", "Run"), DAVEOS_COMMAND(Example, Run, "RUN", "Duplicate")': false,
  'DAVEOS_COMMAND(Example, Run, "HELP", "Reserved")': false,
  'DAVEOS_COMMAND(Example, Run, "bad name", "Invalid")': false,
  'DAVEOS_COMMAND(Example, Run, "", "Empty")': false,
  'core::CommandDescriptor<Example>{"run", "Run", nullptr, "Run"}': false,
  'DAVEOS_COMMAND(Example, Run, "run", "")': false,
}
  if cpp.compiles(registration_check.format(descriptor), dependencies: core_dep,
      name: 'command metadata ' + descriptor) != expected
    error('Command metadata validation did not behave as expected')
  endif
endforeach
names_check = '''
  #include "core/command/command.hpp"
  enum class Event {};
  namespace core = daveos::core;
  template <int N> struct Example : core::Module<Example<N>, Event> {
    static constexpr const char* name() { return N == 0 ? "one" : @0@; }
    static constexpr const char* command_prefix() { return N == 0 ? "one" : @1@; }
    core::Status Run(core::CommandArguments) { return core::Status::ok; }
    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Example, Run, "run", "Run")};
    }
  };
  using List = core::ModuleList<Example<0>, Example<1>>;
  static_assert(sizeof(List) > 0);
  static_assert(sizeof(core::CommandDispatcher<Event, List>) > 0);
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
  namespace core = daveos::core;
  enum class Event {};
  enum class Other {};
  struct Example : core::Module<Example, @0@> {
    static constexpr const char* name() { return "example"; }
    void Task() {}
    static constexpr auto tasks() {
      return std::array{core::TaskDescriptor<Example>{"task", &Example::Task}};
    }
  };
  static_assert(!core::ModuleFor<int, Event>);
  void check(core::SchedulerInterface<Event>& interface, Example& module,
             daveos::platform::fake::Platform& platform) {
    @1@;
  }
'''
foreach operation : [
    'interface.schedule(module, &Example::Task, 1)',
    'interface.cancel(module, &Example::Task)',
    'auto scheduler = core::make_scheduler<Event>(platform, core::ModuleList{&module})',
    'core::CommandDispatcher<Event, core::ModuleList<Example>> dispatcher(core::ModuleList{&module}, interface)']
  foreach event_type, expected : {'Event': true, 'Other': false}
    if cpp.compiles(event_check.format(event_type, operation), dependencies: core_dep,
        name: 'module event ' + event_type + ': ' + operation) != expected
      error('Module event compatibility did not behave as expected')
    endif
  endforeach
endforeach

# Validate typed declarations at the public factory, with positive controls.
typed_check = '''
  #include "core/command/arguments.hpp"
  namespace core = daveos::core;
  struct Example {
    @0@ Run(@1@) { @2@ }
  };
  constexpr auto descriptor = core::command<&Example::Run>("run", "Run" @3@);
'''
foreach variant : [
  ['core::Status', 'std::uint32_t', 'return core::Status::ok;', ', core::arg("value").range(1u, 100u)', true],
  ['core::Status', 'float, std::optional<bool>', 'return core::Status::ok;', ', core::arg("rate").min(0.0f), core::arg("enabled").friendly()', true],
  ['core::Status', 'std::uint32_t', 'return core::Status::ok;', '', false],
  ['core::Status', '', 'return core::Status::ok;', ', core::arg("extra")', false],
  ['void', '', '', '', false],
  ['core::Status', 'std::uint32_t', 'return core::Status::ok;', ', core::arg("value").friendly()', false],
  ['core::Status', 'bool', 'return core::Status::ok;', ', core::arg("value").min(0)', false],
  ['core::Status', 'std::uint32_t', 'return core::Status::ok;', ', core::arg("value").min(-1)', false],
  ['core::Status', 'std::int8_t', 'return core::Status::ok;', ', core::arg("value").max(128)', false],
  ['core::Status', 'std::uint32_t', 'return core::Status::ok;', ', core::arg("value").min(0.5)', false],
  ['core::Status', 'float', 'return core::Status::ok;', ', core::arg("value").range(2.0f, 1.0f)', false],
  ['core::Status', 'float', 'return core::Status::ok;', ', core::arg("value").max(std::numeric_limits<float>::infinity())', false],
  ['core::Status', 'std::optional<int>, int', 'return core::Status::ok;', ', core::arg("optional"), core::arg("required")', false],
  ['core::Status', 'int&', 'return core::Status::ok;', ', core::arg("value")', false],
  ['core::Status', 'int', 'return core::Status::ok;', ', core::arg("")', false],
  ['core::Status', 'std::optional<double>', 'return core::Status::ok;', ', core::arg("value").range(0.5, 2.0)', true],
  ['core::Status', 'std::optional<std::string_view>', 'return core::Status::ok;', ', core::arg("value")', true],
  ['core::Status', 'std::optional<std::uint8_t>', 'return core::Status::ok;', ', core::arg("value").max(256u)', false],
  ['core::Status', 'std::optional<float>', 'return core::Status::ok;', ', core::arg("value").friendly()', false],
  ['core::Status', 'std::optional<bool>', 'return core::Status::ok;', ', core::arg("value").max(1)', false],
  ['core::Status', 'std::string_view', 'return core::Status::ok;', ', core::arg("value").min(1)', false],
  ['core::Status', 'std::optional<std::string_view>', 'return core::Status::ok;', ', core::arg("value").friendly()', false],
  ['core::Status', 'std::optional<std::optional<int>>', 'return core::Status::ok;', ', core::arg("value")', false],
  ['core::Status', 'double', 'return core::Status::ok;', ', core::arg("value").min(std::numeric_limits<double>::quiet_NaN())', false],
  ['core::Status', 'double', 'return core::Status::ok;', ', core::arg("value").min("bad")', false],
  ['core::Status', 'const char*', 'return core::Status::ok;', ', core::arg("value")', false],
  ['core::Status', 'long double', 'return core::Status::ok;', ', core::arg("value")', false],
  ['core::Status', 'core::CommandArguments', 'return core::Status::ok;', ', core::arg("value")', false],
]
  if cpp.compiles(typed_check.format(variant[0], variant[1], variant[2], variant[3]),
      dependencies: core_dep, name: 'typed command ' + variant[1] + variant[3]) != variant[4]
    error('Typed command validation did not behave as expected')
  endif
endforeach

capacity_check = '''
  #include "core/command/arguments.hpp"
  namespace core = daveos::core;
  template <std::size_t... I>
  struct Example {
    core::Status Run(decltype((void)I, std::uint8_t{})...) { return core::Status::ok; }
  };
  template <std::size_t... I>
  consteval auto make(std::index_sequence<I...>) {
    return core::command<&Example<I...>::Run>("run", "Run", ((void)I, core::arg("value"))...);
  }
  constexpr auto descriptor = make(std::make_index_sequence<@0@>{});
'''
foreach count, expected : {'16': true, '17': false}
  if cpp.compiles(capacity_check.format(count), dependencies: core_dep,
      name: 'typed parameter capacity ' + count) != expected
    error('Typed parameter capacity check did not behave as expected')
  endif
endforeach

choices_check = '''
  #include "core/command/arguments.hpp"
  #define MODES(X) X(on, -7) X(once, 10) X(off, 50)
  DAVEOS_ENUM(Mode, std::int16_t, MODES)
  #undef MODES
  enum class Plain { one, two };
  inline constexpr std::array labels{daveos::core::EnumChoice{"start", Mode::on}};
  inline constexpr std::array wrong{daveos::core::EnumChoice{"one", Plain::one}};
  inline constexpr std::array duplicate{daveos::core::EnumChoice{"on", Mode::on}, daveos::core::EnumChoice{"ON", Mode::off}};
  inline constexpr std::array empty_name{daveos::core::EnumChoice{"", Mode::on}};
  inline constexpr std::array null_name{daveos::core::EnumChoice<Mode>{nullptr, Mode::on}};
  inline constexpr std::array<daveos::core::EnumChoice<Mode>, 0> empty{};
  struct Receiver { daveos::core::Status Apply(@0@) { return daveos::core::Status::ok; } };
  constexpr auto descriptor = daveos::core::command<&Receiver::Apply>("apply", "Apply", daveos::core::arg("mode")@1@);
'''
foreach variant : [
  ['Mode', '', true],
  ['std::optional<Mode>', '', true],
  ['Mode', '.choices<labels>()', true],
  ['std::optional<Mode>', '.choices<labels>()', true],
  ['Plain', '.choices<wrong>()', true],
  ['Mode', '.choices<wrong>()', false],
  ['std::optional<Mode>', '.choices<wrong>()', false],
  ['Mode', '.choices<duplicate>()', false],
  ['Mode', '.choices<empty_name>()', false],
  ['Mode', '.choices<null_name>()', false],
  ['Mode', '.choices<empty>()', false],
  ['Mode', '.min(0)', false],
  ['Mode', '.friendly()', false],
  ['std::optional<Mode>', '.choices<labels>().max(1)', false],
  ['Mode', '.friendly().choices<labels>()', false],
  ['int', '.choices<labels>()', false],
  ['bool', '.choices<labels>()', false],
  ['std::string_view', '.choices<labels>()', false],
  ['Plain', '', false],
]
  if cpp.compiles(choices_check.format(variant[0], variant[1]), dependencies: core_dep,
      name: 'enum choices ' + variant[0] + variant[1]) != variant[2]
    error('Enum choice declaration validation did not behave as expected')
  endif
endforeach
