exe = executable('test-events', 'events.cpp', dependencies: test_support_dep)
test('events', exe, suite: 'unit', timeout: 20)
cpp = meson.get_compiler('cpp')
event_check = '''
#include "core/schedule/module.hpp"
#include <string>
namespace core = daveos::core;
struct A { int value = 0; };
struct B {};
struct Throwing { Throwing() noexcept(false) {} };
struct Immutable { const int value = 0; };
struct Other { void Read(const A&) {} };
using Event = @0@;
struct Receiver : core::Module<Receiver, Event> {
  static constexpr const char* name() { return "receiver"; }
  @1@
  static constexpr auto events() { return std::tuple{@2@}; }
};
void check(Receiver& r, core::SchedulerInterface<Event>& s) {
  r.receive(Event{});
  @3@
}
'''
foreach variant : [
  ['std::variant<A,B>', 'void Read(const A&) {}', 'DAVEOS_EVENT(Receiver, Read)', 's.post(A{});', true],
  ['std::variant<A,B>', 'void Read(const A&) const noexcept {}', 'DAVEOS_EVENT(Receiver, Read)', 's.post(Event{B{}});', true],
  ['std::variant<A,B>', 'void on_event(const Event&) {}', '', '', true],
  ['std::variant<A,B>', '', '', '', true],
  ['int', '', '', '', false],
  ['std::variant<A,A>', '', '', '', false],
  ['std::variant<std::string>', '', '', '', false],
  ['std::variant<Throwing>', '', '', '', false],
  ['std::variant<Immutable>', '', '', '', false],
  ['std::variant<A>', '', 'DAVEOS_EVENT(Other, Read)', '', false],
  ['std::variant<A>', 'void Read(const B&) {}', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void Read(A) {}', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void Read(A&) {}', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'int Read(const A&) { return 0; }', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void Read(const A&, int) {}', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void Read(const A&) {}', 'DAVEOS_EVENT(Receiver, Read), DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void Read(const A&) {} void on_event(const Event&) {}', 'DAVEOS_EVENT(Receiver, Read)', '', false],
  ['std::variant<A>', 'void on_event(Event) {}', '', '', false],
  ['std::variant<A>', 'void Read(const A&) {}', 'core::event_handler<&Receiver::Read>("")', '', false],
  ['std::variant<A>', '', '', 's.post(B{});', false],
  ['std::variant<int>', '', '', 's.post(short{1});', false],
]
  if cpp.compiles(event_check.format(variant[0], variant[1], variant[2], variant[3]),
      dependencies: core_dep, name: 'event declaration ' + variant[0] + variant[1] + variant[2] + variant[3]) != variant[4]
    error('Event declaration validation did not behave as expected')
  endif
endforeach
