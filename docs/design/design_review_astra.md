# DaveOS design review — Astra

Reviewed against `ef4ef78` and the working tree on 2026-09-22.

## Assessment and scope

DaveOS has a coherent reason to exist: small cooperative embedded applications,
explicit state machines, predictable ownership, fixed storage, and useful host
models. Its strongest property is that application behavior and peripheral
lifetime are usually visible in ordinary C++ instead of being hidden behind
threads or a framework-managed object graph.

The project is now substantially more than a scheduler. It includes a command
system, console transports, networking, bus controllers, storage, boot/update
policy, watchdog supervision, and provisioning. That breadth is useful, but
**the next design work should make those pieces easier to understand and reuse,
rather than add another layer of generality.**

I reviewed the specifications, rationale, tutorials, public API structure,
scheduler/composition implementation, representative service and adapter code,
STM32 example/build wiring, and test/CI organization. This is an architectural
review across the project, not a line-by-line correctness audit of every driver
or a new qualification run. No builds or hardware tests were rerun for this
review. Hardware conclusions below refer to recorded results and their stated
limits. I did not use the separate Claude review as an input.

## What should remain

- **The execution model.** Run-to-completion callbacks and explicit state
  machines fit the intended applications. Do not turn DaveOS into a partial
  preemptive RTOS.
- **Passive construction and two-stage initialization.** Application-owned
  objects can live at file scope without requiring hardware to be ready during
  construction. Keep the guarantee that all stage1 calls precede stage2.
- **Explicit composition.** `ModuleList`, application-owned logger/transports,
  and `make_application()` are a good balance between convenience and visibility.
  A dependency-injection container or automatic module discovery would obscure
  more than it saves here.
- **Compile-time metadata with modest macros.** Typed commands, event handlers,
  task registration, and periodic tasks catch useful mistakes while keeping
  implementation in C++. Keep the underlying functions accessible.
- **The independent-service pattern.** `net::Service` and its small module
  adapter are a strong example. Inject hardware/time into reusable logic; adapt
  that logic to DaveOS separately.
- **The testing split.** Fake time, file-backed flash/OTP, sanitizers,
  compile-failure tests, real protocol tests, and explicitly opted-in HIL solve
  different problems. Preserve that distinction.
- **The current source layout principle.** Co-located headers and sources,
  component directories, `.hpp` for templated headers, and `.cpp` for C++ are
  appropriate. Another wholesale directory rename would have little payoff.

## 1. Concept: state the product boundary more sharply

### Present DaveOS as a kernel plus optional libraries

`README.md` is approachable, and `RATIONALE.md` explains the intended style well.
The implementation already supports considerable separation. Make the public
architecture equally explicit:

| Layer | Responsibility | Examples |
| --- | --- | --- |
| Foundations | Small reusable value types and mechanisms | Time/status, callbacks, state machines, CRC |
| Cooperative kernel | Initialization, scheduling, events, timers, dispatch context | `core/schedule`, `core/event` |
| Optional services | Application functionality | Logging, commands, networking, storage, update, OTP |
| Hardware adapters | Peripheral/OS mechanisms | Host/fake, STM32 timers, SPI DMA, Ethernet |
| Board and application composition | Pins, resources, policies, selected features | Nucleo support, console demo, starters |

This is a dependency rule, not a proposal to immediately move everything into
five new directories. For example, the existing `core` namespace can remain a
convenient API umbrella while internal dependencies become narrower.

A concrete first improvement is extracting `Time`, `Status`, and other truly
shared value types from `core/platform/platform.hpp`. The state-machine helper,
flash interface, OTP driver and file reader currently include the platform
header for foundational types. They should not need the platform contract to
name a result or timestamp.

Likewise, `core/schedule/module.hpp` includes command argument machinery, and
`application.hpp` includes the dispatcher even for an application without
commands. Optional runtime storage does not imply optional parsing and template
work during compilation. Measure that compile cost, then separate command
metadata from parsing implementation if it is material. Do not claim an
embedded RAM problem merely from an include dependency.

**Acceptance criterion:** a small state machine, a standalone injected driver,
and a task-only application each compile with only the layer they need. Add
small consumer compile checks to preserve those boundaries.

### Keep policy decisions visible

The choices to run every overdue repeat, allow firmware downgrades by installation
order, and leave recovery to applications are intentional. Preserve them rather
than quietly introducing fairness, automatic retries, or version enforcement.

Their costs should be stated near the relevant API. In particular, a permanently
overloaded repeat schedule can prevent idle-time logging and other later-due
work from making progress. The current linear scheduler scans are reasonable
for small task sets; neither a heap nor priorities are justified without
measurements. Add an overload example showing lateness, backlog and diagnostic
loss, so users can recognize the failure mode.

## 2. Usability: make the common path short and the advanced path unmistakable

### Keep one recommended composition path

`docs/01-hello.md` and `make_application()` already offer a good starting point.
Use that path consistently in introductory material. Keep direct Scheduler,
CommandDispatcher and binding APIs in the advanced guide; do not make a new
user choose among equivalent construction styles.

Add three independently buildable, small examples between hello and the full
hardware console:

1. A periodic producer and typed-event consumer.
2. One asynchronous device operation, completion, timeout and explicit reset.
3. A service with a thin module adapter and a command that schedules work.

The purpose is not another API. It is to let users learn one concept without
reading a fully equipped board application.

### Make asynchronous ownership contracts consistent to read

Current APIs legitimately have different shapes:

- `hal::Controller`: accepted start, callback completion, reset.
- `boot::Flash`: start plus polling until a terminal status.
- Update and SD initialization: requests plus `tick()` and observable results.
- FatFs adaptation: synchronous calls over an asynchronous transport.

Do not force them into futures, coroutines, or one giant transaction base class.
Instead give each public asynchronous API the same short contract block:

| Question | Required answer |
| --- | --- |
| Admission | What does rejection change? Can completion occur inline? |
| Ownership | Which descriptors/buffers are borrowed, and until when? |
| Execution | Which context invokes completion? May it start new work? |
| Deadline | Does expiry request cleanup or establish completion? |
| Failure | Which state latches? What explicit operation clears it? |
| Lifetime | What must be quiesced before destruction or shutdown? |

Some headers already answer these very well. Apply the same structure across
all of them and test representative lifetime rules. Retain domain-specific
statuses such as `hal::Status`; map them once at an adapter boundary while
preserving the detailed diagnostic cause. A universal error enum would lose
information and create unrelated dependencies.

### Make waiting harder to misunderstand

The new `core::wait_until()` has the right ownership promise, but its name can
suggest a bounded wait. It can keep running after timeout, stop, or invalid
context until the backend releases buffers. That is essential behavior, not a
minor caveat.

Before this API spreads, consider a more explicit name such as
`wait_for_release()`. State that its return describes the wait, while the
completion object describes the operation. Preserve the first failure and never
return borrowed buffers early. Keep the backend's independent timeout mandatory
in examples; no helper can repair an interrupt path that never completes.

Use direct `ready()`, `pump()` and `now()` calls here unless support for broader
`std::invoke` forms is actually required. The recent question about where the
pump is invoked is useful evidence that a small implementation choice affects
readability.

Keep state machines as the default long-operation model. Present nested yield
as a synchronous-library integration technique. Its additional rules—ancestor
resources, stack depth, events/logs not draining, and completed-iteration
watchdog accounting—deserve one diagram and one worked example. Do not make
ordinary drivers depend on nested execution unnecessarily.

### Reduce unit and configuration surprises

Public scheduling conveniences should prefer chrono durations. Raw microsecond
integers can remain at hardware/wire boundaries, but avoid examples that teach
`schedule(..., 10)` without a unit. A focused API migration is more useful than
adding aliases which still permit every mistaken integer conversion.

Keep `Capacities` as the named configuration mechanism. Document that logger,
transport, network-pool and service capacities live elsewhere: it is not a total
application memory budget. For each bounded queue, show capacity, overflow
behavior and the corresponding diagnostic counter together.

## 3. Simplicity: reduce hidden assembly, not useful static typing

### Stop growing executable C++ inside Meson

`examples/stm32_console/meson.build` now assembles members, initialization,
failure handling, shutdown and IRQ bodies as strings. This achieved the desired
feature selection without application `#ifdef` blocks, but it makes behavior
harder to navigate, refactor and diagnose with normal C++ tools.

Keep Meson selecting sources and a small generated composition manifest. Move
behavior into ordinary feature components with explicit methods. Generate only
includes, selected types/objects, and calls to those methods; do not move the
same complexity into a template metaprogramming framework.

For a new feature, aim for one ordinary C++ component, one dependency declaration,
and a short selection entry. Early watchdog setup and reverse shutdown order
should remain visible at the application boundary. Do not create a generic
lifecycle protocol with many optional hooks until the existing components
actually need it.

### Separate the SD service from its demonstration

`examples/stm32_console/sd_probe.hpp` is now roughly 580 lines. It combines bus
construction, initialization, diagnostics, repeated read-only inspection,
block-device adaptation and commands. Reusable initialization has already moved
into `storage/sd/initializer.h`, but filesystem usability still depends on an
example object named `SdProbe`.

Extract a small SD session/block-device owner into `storage/sd/`, with injected
bus/time and explicit ready/failed/reset requirements. Keep pins, DMA allocation,
inspection output and `sd` commands in the board/demo adapter. Reuse existing
initializer and transport code rather than introducing a competing SD driver.

**Acceptance criterion:** a small non-console application can mount a card
without including anything from `examples/`; the demo still offers the same
inspection and failure diagnostics.

### Keep state-machine discipline without unnecessary machinery

The shared CRTP helper usefully enforces one transition commit and common
statistics. Keep enums narrow, switches explicit, and interrupt handlers limited
to publishing requests/completions. Do not turn snapshots or persistent metadata
into artificial state machines.

State statistics have a cost: two 64-bit counters per state, plus dwell and
other bookkeeping. Measure the total across nested service machines before
adding more instrumentation. If tiny targets or bootloader budgets need it,
make statistics optional through one well-defined policy. Do not add that
policy solely because it is possible.

## 4. Expandability: validate reusable boundaries with a second instance

### Exercise multiplicity before adding discovery

The bus registry is explicitly designed for multiple controllers and devices.
Other components still embody intentional single-instance limits: lwIP NO_SYS
has process-global state; the Nucleo UART adapter targets USART3; storage and
console adapters have bounded ownership and serialization rules.

A useful next expansion test is two instances of the same logical driver,
with distinct names, sharing one controller where appropriate. Demonstrate
independent diagnostics, contention, completion, and reset effects. Then do the
same for two application TCP listeners when that TODO is implemented.

This exposes hard-coded module names, callback routing assumptions and implicit
resource sharing earlier than a module lookup service would. Do not promise
multiple network stacks simply because an adapter is injectable; document the
upstream constraint. Keep C middleware callback routing as a narrow adapter
concern rather than introducing a singleton framework.

### Make board resource allocation inspectable

Hardware composition currently requires knowing timer, DMA, IRQ, RAM section,
chip-select and middleware reservations scattered across board headers and
adapters. Add a compact resource table for each supported board/build profile,
then generate or validate the parts already available as build metadata.

Start with concrete collision checks for resources the application selects.
Do not attempt a generic compile-time peripheral allocator. The current rule
that SPI cleanup must never reset a DMA controller shared with UART is exactly
the kind of constraint that belongs in this table.

### Extend lifecycle checks beyond successful startup

Keep two initialization stages; more numbered stages would not solve ownership
ambiguity. Document explicitly which objects own cleanup after partial startup,
and the dependency order for stopping transports, network services, timers and
hardware. `Application` currently owns scheduler/dispatcher lifecycle, while
STM32 composition performs additional cleanup outside it.

Test partial initialization failures and shutdown with pending I/O at those
boundaries. Do not automatically add a universal `Module::stop()` callback unless
a concrete consumer requires it. Explicit application cleanup may remain the
simplest answer.

## 5. Organization: distinguish current contracts from validation history

### Keep the split specification, reduce repetition

The specification split was worthwhile, but the four owning files now total
about 3,200 lines. `PROJECT.md` alone is about 1,060 lines. File length is not
itself a defect; repeating the same contract in a spec, tutorial, API guide,
header, TODO and validation narrative is the maintenance risk.

Keep:

- `RATIONALE.md`: intent and tradeoffs.
- A short architecture/map page: boundaries and the recommended entry path.
- Owning subsystem specs: normative behavior and explicit non-goals.
- Tutorials: small runnable examples linking to those contracts.
- Qualification records: measured results and remaining limitations.

Move completed experimental narratives from TODO into qualification records as
they are touched. Keep TODO focused on outstanding work and brief links to
completed milestones. A support matrix already exists in the STM32 spec; make
it the current summary instead of creating a competing matrix.

For each qualification record, capture firmware revision, configuration, board,
fixture, tested behavior and limits. Our latest DMA test passes cleanup checks
while card reinitialization fails. A plain green “SD recovery” entry would be
misleading; retain that distinction.

### Make build profiles reproducible

CI has useful coverage of host sanitizers, fake time, disabled logging, both
boards and feature combinations. Preserve it. Add a few named, versioned build
profiles for common workflows: minimal host, fake tests, Nucleo console, and
storage/update validation. These should supply existing Meson options, not
replace Meson with another build system.

Emit the selected features and relevant capacities alongside firmware artifacts.
This would make a hardware result easier to reproduce than reconstructing a
long `meson setup` command from a conversation or local build directory.

Keep feature dependencies clear and early: which options select libraries,
which select example fixtures, which require board resources, and which are
ignored outside their scope. `spi_sd_probe` currently names a diagnostic fixture
but also gates the SD path used by filesystem/OTA composition; separating those
concepts would make configuration clearer.

### Use evidence to decide optimization work

The existing ELF allocation audit and stack painting are valuable. Extend the
report, when practical, to attribute major static objects and section placement.
That will support the planned H755 DTCM/AXI work and explain the cost of queues,
commands, logs and DMA buffers to users.

Report linked allocation symbols separately from observed heap attempts, and
observed stack watermark separately from a worst-case bound. Measure compile
time and minimal firmware size before splitting templates or adding storage
policies. Source-line counts do not establish runtime cost.

## Recommended order

| Priority | Work | Completion criterion |
| --- | --- | --- |
| 1 | Clarify waiting, async ownership and lifecycle contracts | One consistent contract format; wait name/semantics settled; examples distinguish admission, completion and buffer release |
| 2 | Extract reusable SD session from the demo | Standalone small consumer mounts storage without example headers; failure/ownership tests preserved |
| 3 | Simplify STM32 build composition | Behavior is navigable C++; generated content selects components and routes calls |
| 4 | Document resources and measure memory placement | Per-profile board resource map and reproducible memory report; use it to drive H755 DTCM/AXI changes |
| 5 | Tighten foundational dependencies | Small consumer checks for state machine, driver and task-only application; measured compile/size effects |
| 6 | Prove two-instance reuse and document named profiles | Independent devices/listeners work within documented limits; profiles reproduce builds and HIL setup |
| Ongoing | Consolidate contracts and qualification history | Each behavior has one owning contract; support claims distinguish build, simulation and hardware evidence |

These should be separate, reviewable changes. Keep semantic tests stable while
changing organization; do not combine source moves, API redesign and peripheral
behavior changes into one large commit.

## What I would defer

- A general coroutine/task runtime, priority scheduler, service locator or
  automatic dependency graph.
- A universal async-operation abstraction or global error type.
- A new filesystem, automatic SD recovery, or automatic replay of uncertain writes.
- Broad template cleverness to save a few registration lines.
- Security work in this review cycle, as requested. The current lab-oriented,
  unauthenticated console/update boundary must remain explicit; this review is
  not a deployment-security endorsement.
- Physical power-cut qualification already deferred by the user. It remains a
  separate hardware qualification effort, not something an organizational
  refactor or passing host simulation can establish.

The most useful next milestone is a second small application that consumes the
libraries without copying the full console demo. Use that application to judge
whether each proposed abstraction actually makes DaveOS easier to use.
