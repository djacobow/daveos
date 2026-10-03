# Fundamentals decision record

Probed on branch `fundamentals-probe` from `ef4ef78`, 2026-09-22. Inputs:
[Claude review](design_review_claude.md) and [Astra review](design_review_astra.md).

The reviews contain many organizational suggestions that can be made at any
time. This record covers only choices that get more expensive with every new
module, driver or example. API breakage is acceptable: there are no known
outside users. No hardware was run for these probes; nothing here is a
qualification claim.

## Summary

| # | Fundamental | Decision | Lock-in if left |
| --- | --- | --- | --- |
| F1 | Module identity is a type, not an instance | **Change**: per-instance names | High: every service module hard-codes a unique name |
| F2 | `core` dependency layering | **Change (small)**: extract foundation types; **keep** commands in `module.hpp` | Medium |
| F3 | Async ownership contract and waiting | **Change**: rename wait helper, one contract block, one injection rule | Medium, rising with each driver |
| F4 | Library vs demo boundary | **Change**: move SD session and health out of the example | Medium |
| F5 | Composition generated as Meson strings | **Change**: Meson selects, C++ behaves; one `features` list plus named profiles | Medium, rising with each feature |
| F6 | Raw microseconds in public scheduling APIs | **Change**: chrono-only conveniences | Low now, grows with call sites |
| F7 | Lifecycle and shutdown ownership | **Keep** explicit cleanup; add partial-init test | Low |
| F8 | Compile-time metadata machinery | **Keep**; drop `__PRETTY_FUNCTION__` parsing | Low |

Also found while probing (not a fundamental, but fix with F4):
`starters/*/subprojects/daveos.wrap` pin `88f03b5`, about 40 commits behind
HEAD. CI builds the starters against the local checkout, so the stale pin is
invisible to tests.

## F1. Module identity is a type, not an instance

**Question.** Can one module type be registered twice, for example two MCP3425
adapters or two UART consoles?

**Evidence.**
- Registering two objects of one module type fails at compile time:
  `static assertion failed: module names must be nonempty and unique`
  (`core/schedule/module.hpp`, `ModuleList`). `name()` is `static constexpr`.
- The only workaround is a string non-type template parameter so each instance
  is a distinct type. It compiles and already exists privately as
  `testing::NamedModule<TestName>` in `tests/support.hpp`. It duplicates every
  template instantiation per instance.
- Fixed names in library modules: `fs` (`storage/module.hpp`), `ota`
  (`update/module.hpp`), `otp` (`otp/module.hpp`), `i2c`
  (`hal/adapters/i2c_module.hpp`), `uart`/`usb`/`tcp` console transports.
- ISR routing globals are few and confined to adapters: `active_uart`
  (`platform/stm32/console/uart.cpp`), `active_usb` (`usb/usb.cpp`),
  `active_platform` (`examples/stm32_console/appmain.cpp`). These are narrow
  C-callback bridges, not a pattern to remove.
- The runtime already carries names as `const char*`: scheduler
  `Registration::name`, dispatcher `Entry::prefix`, logging `Context::module`.
  `Status::duplicate_name` already exists.

**Decision: change.** Module names become per-instance, supplied at
construction (defaulting to a type-provided name), and uniqueness moves from
`static_assert` to `init()` validation returning `duplicate_name`. Plain
drivers (`drivers/mcp3425.h`) are already instance-safe; only module adapters
change. Keep the adapter globals.

**Cost to change later:** every new service module adds another fixed name and
another consumer relying on type identity.

## F2. Dependency layering of `core`

**Question.** Do small consumers pay for layers they do not use?

**Evidence** (GCC 15, `-fsyntax-only`, host):

| Consumer / header | Project headers | Parse time |
| --- | --- | --- |
| State machine only | 5 (incl. `platform.hpp`, `timer_callback.hpp`) | 0.23 s |
| MCP3425 driver only | 10 (incl. `platform.hpp`, `duration.hpp`) | — |
| Task-only app via `application.hpp` | 20 (incl. dispatcher, arguments) | 1.60 s at `-O2` |
| Same app via `scheduler.hpp` | 17 (still incl. arguments) | 1.59 s at `-O2` |
| `core/platform/platform.hpp` | — | 0.22 s |
| `core/command/arguments.hpp` | — | 0.38 s |
| `core/schedule/duration.hpp` (`<chrono>`) | — | 1.02 s (`<chrono>` alone 0.88 s) |

- `boot/flash.h`, `otp/driver.h`, `watchdog/driver.h`,
  `watchdog/confirmation.h`, `storage/read_file.h`,
  `hal/adapters/daveos.hpp` and `core/state_machine/state_machine.hpp`
  include the platform contract only to name `Time`/`Status`.
- Code size, H563 starter rebuilt against HEAD: 92,316 B text, 22,272 B bss.
  By symbol origin: ST HAL/USB ≈ 34.5 KB, newlib (printf family, `_dtoa_r`,
  malloc) ≈ 26.3 KB, DaveOS and application symbols a few KB. Full console
  image: 190,372 B text.

**Decision.**
- **Change:** move `Time`, `kForever`, `Status`, `InitStage`, `Mode` into a
  foundation header with no platform contract (e.g.
  `core/foundation/types.hpp`); `platform.hpp` includes it. Add consumer compile
  checks (state machine, standalone driver, task-only app) that fail if a
  forbidden header is included.
- **Keep** command metadata in `module.hpp`. Splitting saves at most about
  0.4 s of a 1.1 s parse and nothing measurable at `-O2`; `<chrono>` dominates
  and is a deliberate choice (see F6). No size cost was attributable to it.
- **Not a DaveOS structure issue:** newlib's formatter is the largest
  controllable code cost. That is the existing TODO for an allocation-free log
  formatter, not a layering change.

## F3. Async operation and ownership contract

**Question.** Is there one readable answer to "who owns buffers, when does
completion happen, what does timeout mean"?

**Evidence.**
- `core::wait_until` (`core/schedule/wait.hpp`) keeps pumping past its timeout
  until `ready()`; the comment says so, the name does not. It has **no library
  callers**: only `tests/yield/yield.cpp` and `tests/reliability/reliability.cpp`.
- `scheduler().yield()` has **one** library caller: the SD pump in
  `examples/stm32_console/sd_probe.hpp` wrapping synchronous FatFs.
- Public async shapes in use:
  - `request / tick / result / reset`: `drivers/mcp3425.h`,
    `storage/sd/initializer.h` (converged independently).
  - `start` + callback or `Completion::ready/result`: `hal/controller.hpp`,
    `hal/transaction.hpp`, `storage/sd/transport.h`.
  - `begin / tick / status / abort`: `update/engine.h`.
  - Start + `poll()` until terminal: `boot::Flash` (`boot/flash.h`).
- Three injection styles: struct of function pointers with `void*` context
  (`boot::Flash`, `otp` driver, `watchdog` driver, `storage::BlockDevice`,
  `sd::Transport::Pump`), template backends (`hal::Controller<Backend, Clock,
  Critical>`), CRTP (platform).
- `storage::BlockDevice` returns `bool`, discarding the cause that
  `hal::Status` and the SD transport's diagnostics hold.

**Decision: change.**
- Rename `wait_until` to `wait_for_release`; call `ready()`, `pump()`, `now()`
  directly instead of `std::invoke`. Free to do now: no library callers.
- State machines are the default long-operation model. Nested `yield()` is
  documented as the technique for wrapping synchronous libraries (FatFs) only.
- Adopt the six-question contract block (admission, ownership, execution
  context, deadline, failure latching, lifetime) at the top of each async
  header listed above.
- Name `request / tick / result / reset` as the recommended driver shape; do
  not force existing HAL/flash APIs into it.
- One injection rule: function-pointer structs for runtime-swappable device
  boundaries, templates for hot per-transfer paths, CRTP only for the platform.
- `BlockDevice` operations return `Status` (or a domain status) instead of
  `bool`, mapped once in the adapter.

## F4. Library vs demo boundary

**Question.** Can an application use storage, health, boot or I2C without
including anything from `examples/`?

**Evidence.**
- Host: yes. A fake-platform app using `storage::Module` with
  `platform/host/FileBlockDevice` compiles with no `examples/` include.
- STM32: no. The `BlockDevice` producer for SD lives in
  `examples/stm32_console/sd_probe.hpp` (578 lines, 28 board-specific
  references) alongside bus construction, reset, diagnostics and commands.
- Board-independent code living in the example:

| File | Lines | Board refs | Belongs in |
| --- | --- | --- | --- |
| `health.hpp` | 243 | 0 | library (`watchdog/`) |
| `sd_inspect.h` | 122 | 0 | library or tests (`storage/sd/`) |
| `sd_probe.hpp` session / block-device part | — | — | library (`storage/sd/`) |
| `sd_probe.hpp` pins, DMA, `sd` commands | — | 28 | example |
| `i2c_probe.hpp` | 117 | 21 | example (board wiring) |
| `adc_probe.hpp`, `ota*.hpp`, `otp*.hpp`, `boot.hpp` | 34–90 | 0–2 | example (thin composition) |

**Decision: change.** Extract an SD session / block-device owner into
`storage/sd/` with injected bus, time and pump, reusing
`storage/sd/initializer.h` and `transport.h`. Move `health.hpp` behind the
watchdog dependency. Keep pins, DMA allocation and diagnostic commands in the
example. Acceptance: a second small STM32 application mounts storage and
samples the MCP3425 without any `examples/` include. Update starter wrap pins
and add a check that they are not older than the last API break.

## F5. Composition mechanism

**Question.** Should feature selection keep generating C++ bodies as Meson
strings?

**Evidence.** `examples/stm32_console/meson.build` has 23 feature branches and
emits 14 includes, 16 member declarations, 9 module-list entries, 7
`extern "C"` IRQ handlers, 4 early-init statements (including a lambda), 1
failure statement and 1 stop statement into `composition.hpp.in` /
`irqs.cpp.in`. Early-init and failure hooks exist only for health; stop only
for networking. `spi_sd_probe` gates both the diagnostic fixture and the
filesystem/SD update path.

**Decision: change.**
- Meson emits only includes, member declarations and module-list entries.
- IRQ handlers move into per-feature C++ sources (e.g. `i2c_irqs.cpp`) that
  Meson adds to the build only when selected. No generated function bodies.
- Health's four early-init strings become one method call
  (`health.attach(scheduler, boot)`); the logic lives in C++.
- No generic lifecycle protocol: only two features need hooks. Keep watchdog
  early start and reverse shutdown visible in `appmain.cpp`.
- Split `spi_sd_probe` into a storage/SD selection and a separate diagnostic
  fixture option.

Acceptance: adding a feature is one C++ component, one dependency, and a short
selection entry. The I2C + ADC feature is the first conversion.

**Selection syntax (agreed addition).** Replace the separate feature booleans
and combos (`uart_console`, `usb_console`, `networking`, `tcp_console`,
`fatfs`, `spi_sd_probe`, `spi_sd_dma`, `i2c_adc_probe`, `otp_backend`) with one
array option whose values are restricted to known feature names:

```meson
option('features', type: 'array',
  choices: ['uart', 'usb', 'net', 'tcp', 'sd', 'sd-dma', 'fatfs', 'sd-inspect',
            'i2c-adc', 'health', 'ota', 'otp-emulator', 'otp-h563'],
  value: ['uart', 'usb', 'health'])
```

```sh
meson setup build/h563-storage --cross-file meson/stm32.ini \
  -Dboard=h563 -Dfeatures=uart,usb,health,sd,sd-dma,fatfs
```

- One feature table in Meson, one entry per feature: its dependency, header,
  member declaration, module-list entry, IRQ source file, required features
  and supported boards. Adding a feature means adding one entry.
- Requirements are checked up front with a readable error, for example
  `fatfs requires sd` or `otp-emulator requires -Dbootloader=true`, instead of
  being implied by nested `if` blocks. Unsupported board/feature pairs are
  rejected, not silently skipped.
- Named profiles are small machine files, e.g. `meson/profiles/console.ini`,
  `storage.ini`, `full.ini`, each containing only
  `[project options] features = [...]`. They are layered after the board cross
  file (`--cross-file meson/stm32.ini --cross-file meson/profiles/storage.ini`),
  and `-Dfeatures=` still overrides them. CI and HIL use the profile names.
- The build writes the resolved feature list (plus board and capacities)
  beside the firmware, so a hardware result records exactly what was built.
- `board` and `bootloader` stay separate options: they change layout and
  linking, not just which components are included.
- Consumers to migrate in the same change: `.github/workflows/build.yml`,
  `platform/**/meson.build`, `starters/stm32/meson.build`, and HIL helpers that
  read these options (`tests/hil/conftest.py`, `test_i2c.py`, `test_otp*.py`,
  `sd_cases.py`, `sd_timeout_cases.py`).

## F6. Time units in public scheduling APIs

**Question.** Keep raw-microsecond integers in `schedule` / `timer`
conveniences?

**Evidence.** Raw-integer `schedule`/`timer` calls: 5 outside tests (mostly
`examples/system/system.cpp`), 111 in tests. The untyped member-pointer form
`schedule(module, &M::f, delay)` appears 4 times outside tests and 87 times in
tests. `Time delay` overloads remain in `core/schedule/module.hpp` and
`scheduler.hpp`.

**Decision: change.** Public scheduling and timer conveniences accept chrono
durations only; the typed `schedule<&M::f>` form becomes the only
member-function form. Raw `Time` stays at hardware/wire boundaries and inside
the scheduler. The test migration is mechanical and should be its own commit.

## F7. Lifecycle and shutdown ownership

**Question.** Who cleans up after a partial initialization or a stop?

**Evidence.** Order in `examples/stm32_console/appmain.cpp`: `platform.init`
→ `application.run()` (init + dispatch) → `components.failed()` →
`components.stop()` (console transports, then network) →
`platform.quiesce()`. `Application` owns scheduler/dispatcher; cleanup of
transports is explicit application code. Piecewise tests exist (e.g. "USB
initialization failure releases the callback route"), but no test covers stage1
failing partway through a module list followed by stop of components that
never initialized.

**Decision: keep** explicit application cleanup; no `Module::stop()` hook.
Add a fake-platform test: stage1 failure at module *k* of *n*, then stop of all
components, including those never initialized, with pending HAL I/O.

## F8. Compile-time metadata machinery

**Question.** Is the template machinery paying for itself?

**Evidence.** No DaveOS size cost was attributable to typed commands in the
H563 starter (F2). `HandlerName()` parses `__PRETTY_FUNCTION__` only to label
direct `core::command<&F>()` descriptors; `DAVEOS_COMMAND` overwrites the label
with `#function`. Direct callers exist only in tests
(`tests/commands/*.cpp`, `tests/arm/arm_compile.cpp`).

**Decision: keep** typed commands, event tuples and `TimerCallback` binding.
Drop compiler-text parsing: `command<&F>()` takes an explicit handler label, or
none. Update the tests that assert parsed names.

## Explicitly deferred

Console/OTA security, physical power-cut qualification, a third platform and
written board-support contract, command JSON manifest, docs taxonomy and TODO
restructuring, directory moves, coroutine or priority scheduling, a universal
error type or async abstraction.

## Suggested execution order

One reviewable commit per item; never mix moves with behavior changes.

1. F3 rename and F8 label change (no library callers; smallest).
2. F2 foundation header plus consumer compile checks.
3. F6 chrono-only API, then the mechanical test migration.
4. F1 per-instance module names.
5. F5 composition: I2C + ADC first, then remaining features.
6. F4 SD session and health extraction, second STM32 application, starter pins.
7. F3 contract blocks and `BlockDevice` status results.
8. F7 partial-init test.

## Reproducing the measurements

- Parse cost: `g++ -std=c++20 -I. -Iplatform -Icore -DDAVEOS_LOGGING=1
  -fsyntax-only -H file.cpp` on a one-line file including the header.
- Starter size: `python3 -B tests/starter/stm32.py $PWD <work> h563`, then
  `arm-none-eabi-size` and `arm-none-eabi-nm -S --size-sort -C` on
  `<work>/build/my-device.elf`.
- Two-instance check: two objects of one module type in one `ModuleList`.
- Call-site counts: `grep -rnE "(schedule|timer)(<[^>]*>)?\([^)]*\b[0-9]+\b"`
  over library and test sources.
