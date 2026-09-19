# A small DaveOS application

Copy this directory into a new repository and run the commands below from that
repository's root. It contains one module, a real-time host executable, and a
fake-time test of the same module. Application module constructors are passive;
the scheduler runs stage1 for every module before any stage2 callback.

```sh
meson setup build/host
meson compile -C build/host
meson test -C build/host --print-errorlogs
./build/host/my-app
```

The first setup downloads DaveOS through `subprojects/daveos.wrap`. Its
`revision` is pinned to a tested commit; update it deliberately when upgrading.
For local DaveOS development, place a checkout (or symlink) at `subprojects/daveos`
before setup; no download is then needed. The real-time host platform starts its
timer thread when constructed in `main()`; module initialization waits for `run()`.
These commands need Meson, Ninja, and a C++20 compiler, but do not download
DaveOS's own test dependencies.

The Counter declares its 1 ms repeat with `DAVEOS_PERIODIC`; no init override is
needed. `make_application` owns the scheduler and handles initialization. The
starter has no logging or commands. Add a borrowed logger, CommandSourceList,
or both as factory arguments when needed. Sources bind automatically after both
initialization stages; no separate dispatcher/connect call is needed.

To grow the application, add modules to `ModuleList` and test them on the fake
platform. Keep device access behind injected interfaces so the same module can
later run on an STM32 platform. The repository's STM32 console demonstrates
board startup, linker configuration, peripherals, and optional transports;
this starter intentionally supplies only host/fake build targets.
