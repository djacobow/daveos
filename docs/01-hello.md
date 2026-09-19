# 1. Hello and a periodic task

A module describes its callbacks. A platform supplies time and synchronization.
Application registers modules, runs initialization, and dispatches their work.

The smallest useful program can print once and stop on the host:

```cpp
#include <cstdio>
#include "core/schedule/application.hpp"
#include "platform/host/io.hpp"
#include "platform/host/platform.h"

namespace core = daveos::core;
namespace host = daveos::platform::host;
using Event = std::variant<std::monostate>;

struct Hello : core::Module<Hello, Event> {
  static constexpr const char* name() { return "hello"; }

  static constexpr auto tasks() {
    return std::array{DAVEOS_PERIODIC(Hello, Poll, std::chrono::milliseconds{1})};
  }

  void Poll() {
    std::puts("Hello, DaveOS!");
    (void)scheduler().stop();
  }
};

int main() {
  host::Platform platform;
  Hello hello;
  auto app = core::make_application<Event>(platform, core::ModuleList{&hello});
  return host::run(app);
}
```

This needs no logger or command dispatcher. `Poll` first becomes due 1 ms after
normal dispatch begins. It runs to completion. `host::run` translates Application's
status into a process exit code and prints failures to stderr.

Constructors should store references and metadata. For setup, override
`init(core::InitStage)`: every module finishes stage1 before any begins stage2,
and dispatch begins only after both stages succeed. You do not need an init hook
just to start a periodic task.

Copy the [host/fake starter](../starters/application/README.md) for an independent
Meson project. Its fake-time test advances automatically without waiting for real
time. For an existing example, build and run `examples/hello/hello-host` as shown
in the [README](../README.md); that version also demonstrates buffered logging.

Next: [logging and commands](02-logging-and-commands.md).
