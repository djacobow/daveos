# DaveOS Design Review

Whole-project review focused on concept, usability, simplicity, expandability, and
organization, with prioritized recommendations. Grounded in the repository as of
2026-09-22: roughly 27k lines of first-party library/example code, 13k lines of
tests, 7.2k lines of markdown docs (excluding TODO.md/README.md), 21 Meson build
options, and one flagship example (`examples/stm32_console`) carrying 16 files.

## Concept

**Strong core idea, executed consistently.** Cooperative single-thread scheduler,
CRTP modules with static dispatch, function-pointer erasure only at the one
necessary boundary (`SchedulerInterface`), two-stage init to solve ordering
without a DAG. This pattern repeats cleanly across the command dispatcher, event
handlers, timers, and the HAL. The state-machine unification (commit `c5bd2d9`) is
the best evidence the concept is holding together under growth: five
independently-built state machines (journal, OTA engine/writer/package/protocol,
watchdog confirmation, FileFlash) turned out to share one shape, and someone
noticed and factored it out.

**But the project hasn't stated what it's for, and it shows.** In about a week it
grew from a scheduler into a scheduler + console + networking + A/B
bootloader/OTA + watchdog + OTP + SPI/I2C HAL + filesystem — each proven on
exactly one board first. That's consistent with three different projects: a
minimal portable kernel, a batteries-included embedded OS, or a reference
architecture for teaching cooperative embedded C++. Right now it's drifting
toward the second while still being validated like the third (one board, then
"H755 deferred," repeated every commit). Nothing forces a choice yet, but the
choice affects what "done" means for every subsystem.

**No mental-model document exists.** There's a tutorial ramp
(`docs/01-hello.md` → `docs/04-custom-components.md`), a flat `docs/reference.md`,
a split spec (`docs/spec/*.md`), and subject docs (`docs/yield.md`,
`docs/memory.md`, etc.) — but nothing that draws the one picture of how
module/scheduler/dispatcher/logger/HAL/services relate. A newcomer assembles
that picture by reading five documents.

## Usability

**The command layer is the standout.** `core::arg("led").range(1,3)` plus enum
choices with lazy matching turned "declare a validated CLI command" into close
to the minimum ceremony possible while staying allocation-free, and error
messages name the argument (`argument 'mode': ambiguous choice`).
`make_application` + `Capacities{}` replaced four positional-template overloads
with one readable call. Commit-message discipline (host/ASan/hardware
distinguished, gaps stated explicitly) is itself a usability win for anyone
deciding whether to trust a given commit.

Gaps:

1. **No machine-readable command manifest.** The dispatcher already builds a
   compile-time `ArgumentMetadata` table for every command (name, type, bounds,
   choices) to drive `help`. Exposing that same table over a
   `commands --json`-style command (or a build-time dump) would let a host tool
   auto-generate a Python client, a GUI, or documentation — for very little new
   code, since the data already exists.
2. **The starters aren't the front door.** `starters/application` and
   `starters/stm32` are the fastest path to a working project, but the README
   doesn't lead with them.
3. **No "explain this status" reference.** `core::Status` has grown to roughly
   20 values (`depth_limit`, `invalid_context`, `counter_exhausted`, …). Each is
   explained in scattered code comments; there's no single table mapping status
   to likely cause and fix, which matters more once `-Dlogging=false` removes the
   log-message context entirely.
4. **Test entry points are scattered.** `tools/*.py`, `tests/hil/*`, and
   `tests/*/test_*.py` each own part of "how do I verify this."
   `docs/testing.md` runs 302 lines to explain it.

## Simplicity

**Runtime simplicity is real and audited** — the no-heap/no-exception/
fixed-capacity discipline isn't just claimed, it's checked
(`tools/memory_report.py`, `platform/stm32/memory/no_heap.c`, stack painting,
dispatcher token capacity limits).

**Compile-time complexity is now the dominant complexity budget**, and it's
growing faster than runtime complexity is shrinking. `Argument<Minimum,Maximum,
Friendly,ChoiceSet>`'s fluent builder, `TimerCallback`'s overloaded `OwnerType`
deduction, event-handler validation via `requires` and handler tuples, and
`HandlerName()` parsing `__PRETTY_FUNCTION__` are all individually justified and
tested — but together they mean a contributor fixing a command-dispatch bug
needs fluency in variadic CRTP, `consteval`, concepts, and compiler-specific
string parsing at once. That's a high floor for a project that also wants to be
approachable.

Other simplicity costs:

- **Two status vocabularies survive at the seams.** `storage/module.hpp`
  compares FatFs's own `FR_OK` directly alongside `core::Status` and
  `hal::Status`. Each is locally justified; together they're three enums a
  glue-code author must track.
- **Options sprawl with no supported-combination table.** 21 Meson options,
  several with real interdependencies (`otp_backend=h563` needs the bootloader
  layout; `spi_sd_probe` needs the HAL bus). Nothing states which combinations
  are actually tested — this has had to be reconstructed from commit logs on
  every review so far.
- **A recurring "streaming parser over bounded buffers" shape** appears
  separately in the OTA `Protocol`, `PackageReader`, and the journal, only
  partly captured by the shared `StateMachine`. A `StreamParser` primitive could
  plausibly collapse these into one audited piece.

## Expandability

**The injection pattern generalizes well** — `hal::Controller<Backend, Clock,
Critical, N>` should let a new bus or board be added without touching the
generic HAL, and the command/argument infrastructure gives every new module
`help` and validation for free.

**Board support is the biggest expandability risk.** Every new capability so
far — bootloader, watchdog, SPI/I2C, OTP — was built for H563 first and ported
to H755 in a following commit, with its own hardware bring-up each time.
There's no written "board support package" contract (the minimal set of
flash/CRC/bus/fault/timer interfaces a board must supply). Two boards prove the
*idea* of portability; they don't prove the abstraction, since both are ST,
both are Cortex-M, and both required parallel multi-day implementation effort.
A third, materially different board would be the real test.

Other expandability notes:

- **Async drivers keep reinventing the same shape.** The MCP3425 driver and the
  SD writer each hand-roll a tick/poll loop against `hal::Completion` because
  there's no shared "wait for an async HAL operation" helper — this is already
  a TODO item, and it's the right one to land before a fourth driver repeats
  the pattern.
- **The console has one flat namespacing level** (`board led`, `fs mount`,
  `otp serial`). Fine at a dozen modules; unclear at thirty. The same fix as
  usability item 1 (structured metadata) is also the expandability fix here —
  it opens the door to scripting/JSON output without redesigning the
  dispatcher.
- **Networking is one `net::Service`, one TCP console, one OTA listener.**
  Nothing currently scopes what "a second protocol" or "a second listener"
  would require.

## Organization

**The dependency-graph shape is right**: separate Meson deps (`daveos-core`,
`daveos-console`, `daveos-network`, `daveos-hal`), and `PROJECT.md` was split
into `docs/spec/*.md` at the right moment (at roughly 1800 lines).

Concerns:

1. **`examples/stm32_console` is doing three jobs** — reference example,
   hardware-validation fixture, and kitchen-sink app (16 files: console, boot,
   health, otp, i2c_probe, adc_probe, sd_probe, ota). There's no small "just
   blink + one command" example distinct from "every feature enabled." Split
   these: keep the fully-loaded app as the HIL/integration target under a name
   that says so, and add a genuinely minimal example for the tutorial docs to
   link.
2. **Root-level directories no longer distinguish kernel from services.**
   `core/`, `util/`, `platform/` (the kernel) sit as flat siblings next to
   `boot/`, `update/`, `watchdog/`, `otp/`, `storage/`, `hal/`, `net/`,
   `console/` (services built on the kernel). A `services/` parent directory,
   or at minimum a one-diagram tier chart, would make the dependency direction
   visible without reading `#include`s.
3. **Docs have three overlapping systems**: numbered tutorials, subject guides,
   and formal specs, plus a flat reference and a separate
   `docs/application-guide.md` that looks like it predates the `01-04` split
   and was never retired or merged. There's no index stating which doc type
   answers which question (tutorial vs how-to vs reference vs spec).
4. **`TODO.md` is a 270-line chronological log, not a roadmap.** It's an
   excellent record of what happened; it's not a plan, since related items
   (e.g., all of OTA's remaining hardening: signing, rollback protection,
   power-cut testing) are scattered across commit order rather than grouped.
5. **Test/tool responsibility is split three ways** (`tools/`, `tests/hil/`,
   `tests/*/test_*.py`) with no cross-linking README saying which is library
   code versus operator-facing CLI.

## Top recommendations, prioritized

1. **Write a one-page concept/mental-model doc** (and put it first in the
   README) — what DaveOS is, the module/scheduler/dispatcher/HAL relationship
   as one diagram, and explicitly state the project's identity (minimal kernel
   vs batteries-included OS vs reference architecture). This is cheap and
   unblocks most of the organization/concept issues, which are really "no map
   exists yet."
2. **Write down the board support package contract** as a checklist (flash,
   CRC, bus critical-section, fault/reliability, timer) before board #3. This
   is the highest-leverage expandability fix, since it's currently implicit in
   four separate `platform/stm32h5|h7/*` implementations.
3. **Expose command metadata as structured output** (a JSON dump of the
   existing `ArgumentMetadata` tables). One change serves usability (tooling,
   discoverability) and expandability (namespacing/scripting groundwork) at
   once.
4. **Consolidate the docs taxonomy**: retire or merge `application-guide.md`
   into the `01-04` series, add an index page distinguishing
   tutorial/how-to/reference/spec.
5. **Split the flagship example**: a minimal blink+console example for
   tutorials, keep the full-feature app as the named HIL/integration fixture.
6. **Restructure `TODO.md` into a grouped roadmap** (per-subsystem sections)
   separate from the historical validation log, or move the log to a
   CHANGELOG and keep TODO.md as pure plan.
7. **Add a shared async-wait helper for HAL drivers**, since it's already
   flagged and the third driver will otherwise duplicate the pattern a third
   time.
8. **Add a supported-configuration table for Meson options**, so "is this
   combination tested" doesn't require reading commit history.
