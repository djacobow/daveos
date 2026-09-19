# 2. Logging and commands

Keep the same module/platform/Application structure. Add a subscriber to receive
log records, and a command source to submit complete input lines. Logging and
commands are independent: either, both, or neither can be present.

For host output, use the standard sink rather than writing a printf callback:

```cpp
#include "core/logging/logger.hpp"
#include "platform/host/io.hpp"

auto logger = core::make_logger(platform, core::SubscriberList{
    daveos::platform::host::stdout_subscriber()});
auto app = core::make_application<Event>(platform, modules, logger);
```

A module logs with `I_("started")`, `W_(...)`, or the other severity macros.
Records capture the timestamp and caller context immediately; subscribers receive
them in scheduler idle time. The host sink prints the standard time/severity/name
prefix, appends a newline, and flushes stdout. Do not add a newline to log formats.

Declare commands on the module that implements them:

```cpp
static constexpr auto commands() {
  return std::array{DAVEOS_COMMAND(Greeter, "hello", Hello, "Print a greeting")};
}

core::Status Hello(core::CommandArguments args) {
  if (!args.empty()) {
    return core::Status::invalid_argument;
  }
  I_("Hello!");
  return core::Status::ok;
}
```

If the module name is `greeter`, the command is `greeter hello`. The dispatcher
handles tokenization, quoting, abbreviated names, and the complete `help` tree.
Handlers receive argument views valid only during that callback.

A transport owns a `core::CommandSource`. Pass its source to Application:

```cpp
auto sources = core::CommandSourceList{console.command_source()};
auto app = core::make_application<Event>(platform, modules, logger, sources);
```

Application binds sources after both initialization stages. The transport calls
`source.dispatch(line)` from a scheduled callback; interrupt/reader-thread code
must buffer incoming bytes first. Removing the logger argument keeps commands
active but makes log-based responses silent. `-Dlogging=false` removes logging
calls and their argument evaluation throughout the build.

Try the [interactive host console](../examples/console/console.cpp):

```sh
./build/host/examples/console/console-host
```

Enter `help`, `console echo "hello world"`, then `console exit`.
The example uses `host::stdout_subscriber()` and `host::run(application)`.

Next: [a hardware console](03-hardware-console.md).
