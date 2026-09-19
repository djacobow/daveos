# A small DaveOS application

Copy this directory into a new repository. It contains one module, a real-time
host executable, and a fake-time test of the same module. All constructors are
passive; the scheduler runs stage1 for every module before any stage2 callback.

```sh
meson setup build/host
meson compile -C build/host
meson test -C build/host --print-errorlogs
./build/host/my-app
```

The first setup downloads DaveOS through `subprojects/daveos.wrap`. Pin its
`revision` to a tested commit for reproducible application builds. For local
DaveOS development, place a checkout (or symlink) at `subprojects/daveos`
before setup; no download is then needed. These commands need Meson, Ninja,
and a C++20 compiler, but do not download DaveOS's own test dependencies.

The starter has no logging or commands. Add an application-owned logger to
`make_scheduler` when needed. For commands, construct a dispatcher and register
`core::make_command_binding<Event>(sources)` as a module; call its
`connect(dispatcher)` before `run()`. It binds sources in stage2.

To grow the application, add modules to `ModuleList` and test them on the fake
platform. Keep device access behind injected interfaces so the same module can
later run on an STM32 platform. The repository's STM32 console demonstrates
board startup, linker configuration, peripherals, and optional transports;
this starter intentionally supplies only host/fake build targets.
