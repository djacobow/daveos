exe = executable('test-state-machine', 'state_machine.cpp', dependencies: test_support_dep)
test('state-machine', exe, suite: 'unit', timeout: 20)

cpp = meson.get_compiler('cpp')
foreach declaration : [
  'enum State { idle }; struct Machine : core::StateMachine<Machine, State, idle, 1> { void Step(State, State&) {} };',
  'enum class State { idle }; struct Machine : core::StateMachine<Machine, State, State::idle, 0> { void Step(State, State&) {} };',
  'enum class State { idle }; struct Machine : core::StateMachine<Machine, State, static_cast<State>(-1), 1> { void Step(State, State&) {} };',
  'enum class State { idle }; struct Machine : core::StateMachine<Machine, State, State::idle, 1> {};',
  'enum class State { idle }; struct Machine : core::StateMachine<Machine, State, State::idle, 1> { int Step(State, State&) { return 0; } };',
]
  if cpp.compiles('#include "core/state_machine/state_machine.hpp"\nnamespace core = daveos::core;\n' + declaration + '\nint main() { Machine m; (void)m.tick(); }',
      dependencies: core_dep, name: 'reject invalid state machine declaration')
    error('Invalid state machine declaration unexpectedly compiled')
  endif
endforeach
