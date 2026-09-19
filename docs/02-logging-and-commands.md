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
  return std::array{DAVEOS_COMMAND(Greeter, Hello, "hello", "Print a greeting")};
}

core::Status Hello() {
  I_("Hello!");
  return core::Status::ok;
}
```

If the module name is `greeter`, the command is `greeter hello`. The dispatcher
handles tokenization, quoting, abbreviated names, and the complete `help` tree.
A zero-parameter handler rejects extra arguments automatically. For typed input:

```cpp
core::Status Sample(float rate, std::optional<std::uint32_t> count) {
  return StartSampling(rate, count.value_or(100));
}

// In commands():
DAVEOS_COMMAND(Sampler, Sample, "sample", "Collect samples",
               core::arg("rate").range(0.5f, 1000.0f),
               core::arg("count").min(1u))
```

The dispatcher checks count, conversion, and inclusive bounds before calling the
handler. Only trailing parameters may be optional. Booleans accept `true/false`;
`core::arg("enabled").friendly()` also accepts `1/0`, `on/off`, `yes/no`,
`enable/disable`, `high/low`, and `set/clear`, ignoring ASCII case. Unknown values
are errors. Integers accept decimal and explicit `0x`/`0b` prefixes; floats accept
finite decimal/scientific values. Metadata mismatches fail at compile time.

For unconverted text, use `std::string_view` with `core::arg("text")`.
For example, `Status Label(std::string_view text,
std::optional<std::string_view> suffix)` receives borrowed tokens after the normal
quote/escape processing. An omitted suffix is `std::nullopt`; `""` supplies an
empty string. Neither view should be retained after the handler returns.

For unusual syntax, retain a `core::CommandArguments` handler and omit argument
metadata. It validates its own input. Raw argument views and typed
`std::string_view` parameters are borrowed only during the callback.

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
