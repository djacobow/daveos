# DaveOS design review — follow-up

Reviewed HEAD `cca30acecb941550ca6c6a9aa2c20e210c2d9e8e`, 2026-09-23.

## Assessment

The architectural cleanup substantially improved DaveOS. The distinction between
portable libraries, platform integration, executable applications, and examples
is now visible in both the directory structure and the code. SD storage and
health are usable outside the flagship example. Public scheduling uses explicit
units. Module identity is no longer inherently tied to its C++ type. These are
meaningful improvements to reuse, not merely rearrangements.

I would keep the present architecture. The next work should make its existing
rules hold consistently as components are added. The most concrete remaining
problems are **a broken pinned storage starter, incomplete propagation of
instance naming, and missing CI builds for recent optional integrations**.
They can be fixed without introducing another framework layer.

This review uses the earlier [Astra review](design_review_astra.md),
[Claude review](design_review_claude.md), and especially the accepted/rejected
choices in [the fundamentals decision record](fundamentals.md). Deferred ideas
are not treated as outstanding promises to implement.
I subsequently compared these findings with the other
[review of the same HEAD](review-2026-09-23.md), as requested; the comparison
below records agreements and differences rather than silently combining them.

## Scope and fresh evidence

I examined the current API and library boundaries, scheduler/application
composition, representative async services and their adapters, starters, Meson
feature selection, CI, tests, and documentation. This is a design review, not a
line-by-line audit of every driver or vendor dependency.

Fresh checks for this review:

- Host ASan/UBSan suite: **44/44 passed** in `build/layout-host`. The first
  sandboxed attempt encountered LeakSanitizer restrictions; the rerun outside
  the sandbox passed normally.
- Reproduced the storage starter's pinned-dependency configuration failure
  using a local archive of its exact pinned commit, without fetching a moving
  branch or replacing it with HEAD.
- Compiled and ran a small application containing two `DebugDisplay` objects
  with separate targets: initialization returned `duplicate_name`.
- Inspected CI configuration; did not fetch or assert the result of a new
  GitHub Actions run.

No ARM builds or hardware tests were rerun for this review. Hardware statements
below refer to recorded project evidence, not new qualification. Reproduction
artifacts are under ignored `build/design-review-20260923/`; the suite log is
`build/design-review-20260923-host.log`.

## Follow-through on the previous reviews

| Earlier decision or concern | Current assessment |
| --- | --- |
| F1: per-instance module identity | Core support and tests landed; storage and console adapters use it. Several reusable adapters still do not expose it, including new ones. See finding 2. |
| F2: foundation dependencies | Landed, with small consumer builds and forbidden-header checks. Keeping command metadata in `Module` was a measured decision; I would not reopen it. |
| F3: async ownership and wait naming | `wait_for_release` now says what it guarantees, and important async headers explain admission, ownership, context, deadlines, failure, and lifetime. `BlockDevice` retains status information. |
| F4: reusable storage and health | SD `Session`, platform SD wiring, and health were extracted. The second storage application no longer needs example-private implementation. The starter-pin acceptance condition remains incomplete. |
| F5: composition | Feature profiles, requirement checks, and generated selection replaced much of the previous generated behavior. IRQ implementations are ordinary C++. Keep this approach. |
| F6: time and task APIs | Chrono durations and typed task selection landed. The old bare-integer convenience concern is resolved. |
| F7: lifecycle | Explicit cleanup was retained deliberately; partial-initialization tests now exercise that policy. No need to invent a universal module shutdown hook. |
| F8: metadata | Compiler-text handler-name parsing was removed. Keep typed commands, event descriptors, and bound callbacks. |
| Repository organization | `lib/`, `platform/`, `apps/`, and `third_party/` communicate useful boundaries. Another broad directory move would have little value. |
| Hardware coverage | The old blanket H755 coverage objection is obsolete. H755 has substantial recorded reliability, storage, and update coverage; SSD1306 now exercises I2C writes. Remaining qualification must be described per operation. |

## Findings

### 1. The shipped storage starter does not work against its pinned dependency

**Priority: high — new-project entry point.**

All three starter wraps still pin
`88f03b580c757d8db389d9a7d8c4fe9c7ffc8660`. The storage starter requests
`features=uart,sd,fatfs`, but that commit predates the `features` option and the
extracted storage support it uses.

Reproducing configuration with the current `starters/stm32_storage` and the
actual pinned checkout fails with:

```text
ERROR: Unknown option: "daveos:features".
```

The current starter tests cannot catch this:
`tests/starter/starter.py:14` and `tests/starter/stm32.py:16` install a symlink to
the working repository as `subprojects/daveos`, then disable downloads. That
is a useful HEAD-consumer test, but it is not a test of what a user obtains from
the wrap. The same old pin in the other starters deserves testing; I am not
claiming that all three have the identical failure.

**Recommendation:** keep the existing local-checkout tests and add a separate
pinned-consumer check. Update starter pins to a published compatible revision
and verify each starter with that dependency. A release process can update the
pins after publishing the implementation; a commit need not name itself.

**Acceptance:** a copied starter configures, builds, and runs its applicable
tests/programming-plan checks without silently substituting HEAD. Pin freshness
should be checked against API compatibility, not simply “must equal HEAD.”

### 2. Instance naming works in the core but is not a library-wide convention yet

**Priority: medium — expandability and consistency.**

`core::Module` accepts an instance name through its protected constructor, and
the scheduler validates names at initialization. That is the right mechanism.
But consumers can use it only when an adapter forwards the argument.

Examples that currently do not:

- `lib/drivers/adapters/ssd1306.hpp:16`: fixed `display`.
- `lib/drivers/debug_display.hpp:37`: fixed `debug_display`.
- `lib/net/file_transfer.hpp:21`: fixed `files`, even though its server port is
  configurable.

Two display renderers aimed at independent targets fail initialization with
`duplicate_name`; I reproduced this with the current code. Wrapping the types
to work around naming is precisely the ceremony F1 was intended to remove.
The current single-display application is unaffected.

**Recommendation:** give reusable module adapters an optional instance-name
argument, defaulting to their current names, and forward it to the base. Audit
older adapters as well, distinguishing genuinely singleton hardware services
from reusable objects. Do not add a registry, mutable names, or discovery.

**Acceptance:** tests register two real library adapters of the same type under
distinct names and check routing/statistics or logs, not only two synthetic
test modules. Require the same check for future reusable adapters.

### 3. CI's feature coverage has fallen behind feature selection

**Priority: medium — regression prevention.**

`.github/workflows/build.yml` exercises useful combinations: both boards,
logging disabled, independent console transports, networking, SD/FatFs, DMA,
OTA, and H563 ADC support. However, none of its selected configurations includes
`ssd1306` or `file-transfer`.

Host tests cover substantial portable behavior, and the recorded local ARM/HIL
runs are valuable. They do not ensure that later commits continue to compile
these features' actual STM32 composition. In particular, filesystem and
networking can each remain green while their file-service integration breaks.

**Recommendation:** add a few representative build profiles covering these
integrations on both boards and the shared ADC/display bus on H563. Use the
existing resolved-feature metadata to check that every supported selectable
feature appears in at least one CI build, or has an explicit exception.

Do not build the entire Cartesian product. Combine all-features coverage with
small dependency-boundary configurations, and reserve physical qualification
for explicit HIL jobs.

**Acceptance:** CI compiles the file-transfer module with its network/storage
dependencies and the display module with its real board wiring. Adding a new
feature requires adding coverage or recording why it is deferred.

### 4. The central support matrix is already diverging from the evidence

**Priority: low — documentation accuracy.**

`docs/spec/stm32.md:95` still says H755 I2C hardware is pending. In contrast,
`docs/ssd1306.md:103` and the TODO record actual H755 ACK scanning,
initialization/page writes, and visual confirmation of the text display.
Repeated START, read behavior, stretching, and recovery qualification remain
separate questions; the new evidence does not justify claiming all I2C works.

**Recommendation:** update the matrix to describe the tested operations and
link their detailed records. Prefer one current summary per subsystem over
duplicating run histories across spec, reference, and TODO. This is a targeted
consistency fix, not a proposal to reopen the deferred documentation overhaul.

## Assessment by design category

### Concept

The current rationale answers the earlier “what is this project for?” concern:
DaveOS is an opinionated cooperative framework for the author's embedded
applications. A small scheduler plus optional, independently useful libraries
is consistent with that purpose. It does not need to choose between being only
a kernel and becoming a general-purpose RTOS.

Preserve passive construction, two-stage initialization, application ownership,
and explicit state machines. Keep nested yield as an adaptation technique for
synchronous libraries, not the default way to implement drivers.

A short architecture picture would still help newcomers: platform supplies
execution primitives; Application composes scheduler/commands; module adapters
connect portable services to scheduled callbacks; concrete board wiring owns
the peripherals. Link existing detailed contracts rather than writing another
parallel specification.

### Usability

Typed commands, enum choices, explicit durations, `make_application`, and the
four-step tutorial are strong. The storage starter is especially valuable as a
demonstration that users can build something besides the large console demo.
Fixing its shipped dependency is more important than adding another convenience
factory.

Document the common extension recipe in one short checklist: choose the library
dependency, construct its injected resources, supply an instance name, register
the adapter, and state its cleanup obligations. Show a real two-instance
example once finding 2 is addressed.

### Simplicity

The extraction of SD `Session` and health removes duplicated application work.
The new SSD1306 driver also demonstrates the right split: device operation,
scheduler adapter, and periodic content rendering are distinct responsibilities.

Keep the different status domains and injection forms where they express real
boundaries. A universal async base class, service locator, error enum, or generic
resource manager would add machinery without resolving the concrete findings.

`wait_for_release` now clearly admits that buffer ownership can outlast the
caller's deadline. It currently has test callers rather than library callers;
that is not a reason to force SD's specialized pump through it. Shared policy
and accurate contracts matter more than making every loop use the same helper.

### Expandability

Recent additions exercise the intended seams: a second I2C device, another TCP
service, and a display renderer consuming application-supplied content. The
interfaces accommodate these without scheduler changes. That is good evidence
for the present design.

The next useful tests should cross those seams. For example, exercise file
upload while console/network polling continues, with disconnect during a
yielding filesystem operation; verify lease release and health behavior. Test
the network module's graceful stop while dispatch is still available.
`FileTransferModule` explicitly requires continued polling after `stop()`;
shutdown after the scheduler has already stopped cannot satisfy that contract.
The current terminal STM32 failure path is not evidence of a general graceful
shutdown facility. Keep that distinction explicit rather than adding a global
lifecycle protocol prematurely.

Before allocating another DMA channel or peripheral, add a compact resource
table to the board/profile documentation: controller, pins, IRQ/DMA channel,
memory section, and owning component. Existing memory reports and stack
measurements are useful; extend their attribution as needed instead of making
unmeasured placement changes.

### Organization

The new top-level layout is an improvement and should remain stable. Continue
testing library consumers without example-private headers. The existing
consumer checks should grow with new boundary examples, such as the standalone
SSD1306 driver, rather than becoming exhaustive include policing.

Generated composition is now understandable as selection plus wiring. Keep
behavior in C++ and short lifecycle calls in the selection table. There is no
present need for a new configuration language or dependency graph engine.

Documentation organization is serviceable. Correct stale claims now; defer
large taxonomy changes as agreed. Avoid using this review as a reason for a
second broad repository reorganization.

## Recommended order

1. Repair and test the published starter workflow.
2. Complete instance-name forwarding and test actual reusable adapters.
3. Add CI builds for recent integrations and correct the support matrix.
4. Add focused cross-feature lifetime/liveness tests as those services evolve.
5. Add the small architecture picture and resource tables when documenting the
   next application or peripheral.

## Comparison with the other current-HEAD review

We agree on the two most actionable issues: starter pins and missing CI feature
builds. We also agree that the new layout should stay, the console can remain an
integration example, and new drivers do not justify a generic lifecycle or
driver framework.

I would qualify several of its conclusions:

- **Publishing is not currently the pin blocker.** At the time of this review,
  `git rev-list --left-right --count origin/main...HEAD` reports `0 0`.
  That checks the locally recorded remote-tracking ref, not a fresh remote
  query. The statement that main is 24 commits ahead does not match this
  checkout. Also, the demonstrated configuration failure is specifically the
  storage starter; identical pins alone do not prove all starters fail.
- **Instance identity is partially closed.** Core support is complete, but
  new adapters still need to expose it, as demonstrated in finding 2.
- **Filesystem access is deliberately controlled by mount mode.** Read-only
  mounts already reject mutations; mounting read-write enables them. A separate
  network-write enable would be an additional policy, not the first gate.
  Because the unauthenticated console can already issue filesystem commands,
  such a gate would not establish authentication. Security was explicitly
  deferred by the user and the decision record. Keep the lab limitation clear;
  do not make another security mechanism a prerequisite for this cleanup.
- **The I2C lease is a real policy boundary, not yet evidence of a HAL defect.**
  `examples/stm32_console/i2c_probe.hpp` uses a task-context boolean lease for
  multi-transaction client sequences and scans. The HAL serializes individual
  transactions. Those are different responsibilities. Lack of queueing or
  fairness is consistent with the accepted caller-managed model. A missed
  release would be a bug, but this review did not reproduce one. Add tests for
  ADC/display/scan contention and release after failure. If reuse warrants a
  shared lease helper, keep it separate from transaction arbitration; adding a
  third client alone does not require a queue or registry.
- **Deferred work should remain labeled deferred.** Power-cut qualification,
  a third-board contract, command-manifest export, and TODO restructuring were
  explicitly postponed. They matter when their use case becomes active, but
  their absence is not a new architectural regression. The qualification
  backlog must constrain claims of readiness, not automatically dictate the
  next implementation task.

Security, physical power-cut campaigns, a third-platform BSP contract, command
JSON export, and wholesale documentation restructuring remain explicit deferred
work. This review does not turn those decisions into new blockers. Nor does it
reopen intentional version-downgrade support or recommend automatic recovery
where the application currently owns that policy.
