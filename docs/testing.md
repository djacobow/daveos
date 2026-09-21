# Testing

C++ tests use Catch2 and run through Meson. Python checks use pytest, including
factory/package tooling, programming plans, host process behavior, consumer
builds, and H563 hardware-in-the-loop (HIL) tests. Existing unittest assertions
remain compatible with the pytest runner.

## Portable tests

Install the Python test dependencies into your development environment:

```sh
export UV_CACHE_DIR="$PWD/build/uv-cache"
uv venv build/venv
source build/venv/bin/activate
uv pip install meson ninja -r requirements-test.txt
meson test -C build/host --print-errorlogs
python3 -B -m pytest -q
```

Meson supplies the built executables to the host process tests and selects the
external starter build. A direct pytest run executes portable tooling tests;
checks needing a Meson-built executable or explicitly selected starter skip.
It does not import Watcher or access hardware. Fixture-generating scripts remain
build tools rather than collected tests. CI installs the same pinned pytest.

To select a consumer build explicitly:

```sh
python3 -B -m pytest tests/starter/test_starter.py --starter-board host
# ARM toolchain and board dependencies must already be available:
python3 -B -m pytest tests/starter/test_starter.py --starter-board h563
```

Use `python -B` to keep Python bytecode out of the source tree. pytest's cache
lives under `build/pytest-cache/`.

## H563 HIL prerequisites

- NUCLEO-H563ZI connected through ST-LINK (SWD and its UART).
- CN13 USB-C data connection to the host.
- CN14 Ethernet connected to a DHCP LAN reachable from the host.
- ARM compiler, GDB, OpenOCD, Python 3.11+, and the pinned HIL dependencies.
- A DaveOS H563 build with bootloader, UART, USB, networking and TCP console.

Every selected hardware test **mass-erases main flash and programs/verifies the
factory image** before it runs. This includes filtered and single-test runs;
there is no reuse-installed-firmware option. Factory provisioning clears both
slots and metadata, installs confirmed A, invalidates the old retained fault
record in RAM, clears bank-B emulated OTP, and leaves real OTP untouched. Closing
other UART/USB terminals and TCP console clients avoids competing readers or
the console's single-client limit.

```sh
export UV_CACHE_DIR="$PWD/build/uv-cache"
uv venv build/hil-venv
# Reinstall Watcher to honor commit-pin changes even with the same package version.
uv pip install --python build/hil-venv/bin/python --reinstall-package watcher \
  -r requirements-hil.txt

meson setup build/boot-h563 --cross-file meson/stm32.ini \
  -Dexamples=true -Dbootloader=true -Dnetworking=true -Dusb_console=true -Dtcp_console=true
cp tests/hil/h563.toml.example build/hil.toml
```

Edit `build/hil.toml` with the ST-LINK UART and DaveOS USB paths from
`/dev/serial/by-id/`, the build directory, and GDB path. Keep the ARM toolchain's
`bin/` directory on `PATH` for Meson. The suite recompiles the selected build
before programming, so the factory HEX, both slot ELFs and OTA package match.

Start a dedicated OpenOCD server for this board if one is not already running:

```sh
~/install/stmicro/openocd/bin/openocd \
  -f interface/stlink-dap.cfg -c 'transport select dapdirect_swd' \
  -f target/stm32h5x.cfg -c 'adapter speed 1800' \
  -c 'reset_config srst_only srst_nogate connect_assert_srst'
```

The configuration defaults to local GDB port 3333 and Tcl port 6666. HIL uses
this existing server; it does not stop another debugger or start a second
OpenOCD process. Close GDB before running the suite.

Both Nucleo boards may remain attached. Select each ST-LINK explicitly: set
`-Dprobe_serial=<ST-LINK-serial>` on its Meson build for programming targets,
and add `-c 'adapter serial <ST-LINK-serial>'` when starting its OpenOCD server.
Use `/dev/serial/by-id/` paths rather than numbered `ttyACM` devices. Concurrent
OpenOCD servers need distinct GDB, Tcl, and Telnet ports; the HIL TOML must name
the ports and UART/USB paths belonging to the H563. H563 and H755 have separate suites; the runner skips tests for the other board.
Select `tests/hil/h563` or `tests/hil/h755` when running a single board suite.

```sh
build/hil-venv/bin/python -B -m pytest --hil --hil-config build/hil.toml \
  tests/hil -q --junitxml=build/hil/results.xml

# Shorter selections still start each test from factory firmware:
build/hil-venv/bin/python -B -m pytest --hil --hil-config build/hil.toml \
  tests/hil -m 'not slow' -q
build/hil-venv/bin/python -B -m pytest --hil --hil-config build/hil.toml \
  tests/hil/h563/test_watchdog.py -q
```

One fixture owns board access for the run. HIL rejects pytest-xdist and holds
an exclusive lock against concurrent runs in this checkout. Tests execute
serially and must not depend on their order. Missing prerequisites or failed
programming are setup errors, not skipped tests. Ordinary CI runs portable
checks only; HIL requires explicit selection and the physical board.

## Coverage and diagnostics

| Tests | Checks |
| --- | --- |
| Console | UART/USB/TCP commands and timers, software reset, USB reconnect, application/bootloader version identity |
| Network | 1,400-byte ping and repeated TCP reconnect |
| UART | Two stress cycles: bursts, maximum-length lines, recovery, TX counters |
| Faults | UsageFault, BusFault, MemManage and HardFault frame/CRC/status; valid PSP, stack-limit overflow, inaccessible PSP; reset with core debugging disabled |
| Watchdog | Latched task-progress failure; startup-hang capture and rollback of unconfirmed B; interrupt-masked hang resets without a new frame |
| OTA | Disabled listener, corrupt chunk, disconnect/timeout/disable/reset/link-loss interruption, restart from zero, pending-image replacement, A-to-B-to-A updates with concurrent timers |
| Boot metadata | Journal rollover and sector reclamation; both eligible images failing CRC, reset loop and factory recovery |

Watcher manages asynchronous text streams and their cleanup. Binary OTA keeps
its protocol client; byte-level UART stress uses a helper under `tests/hil/h563/`
with an exclusive UART lease. GDB experiments stop at known execution points
and preserve fault records before resuming reset. Teardown releases connections
and resets the board, including after a failed assertion. A subsequent test
always reprograms factory state.

Artifacts live under `build/hil/`: a build log, configuration and SHA-256 artifact
manifest, per-test raw transport transcripts, OpenOCD/GDB logs, retained records,
and the requested JUnit report. Fixed artifact paths describe the latest run;
archive that directory before another run if its evidence needs preserving.
The unattended fault test disables core debugging while leaving ST-LINK physically
connected. Link-loss injection powers down the Ethernet PHY through its management
register; it does not unplug the cable. Physical power interruption remains
deferred; reset tests do not substitute for power-cut qualification. Cable-unplug
tests, LED appearance and button presses are not automated by this suite. H755
uses the separate suite below for reliability and, with a bootloader build, A/B OTA.

## Version identity checks

Application major/minor come from the top-level Meson project version. Bootloader
major/minor use `boot_version_major` and `boot_version_minor`. Both stamps include
the Git commit, dirty flag, and `version_build`; its local default is `4294967295`,
displayed as `local`. `board version` prints application identity, and the
bootloader prints its identity on UART before selecting an image.

CI passes `GITHUB_RUN_NUMBER` as `-Dversion_build` and checks the generated
application and bootloader JSON stamps against the OTA header:

```sh
python tools/verify_version.py --build build/boot --number "$GITHUB_RUN_NUMBER"
```

Portable `version-tool` tests exercise clean/dirty Git state, local/CI build
numbers, invalid values and artifact mismatches. A separate build can override
bootloader major/minor to verify they remain independent of application versions.

### OTP emulator selection

`tests/hil/h563/test_otp.py` requires `-Dotp_backend=flash-emulator`; it skips when
that backend is absent. It covers all console transports, exact deduplication,
confirmation rejection, reset retention, exhaustion, factory clearing, torn-lock
recovery, bidirectional OTA retention and serial writes during OTA. Emulator
fault injection is separate from physical OTP and power-interruption validation.

### Real OTP is read-only in automation

The DUT may already contain records or device keys. `test_otp_hardware.py` runs
only with the real H563 backend and compares per-block read classifications,
fingerprints and locks across repeated reads and reset. It makes no assumption
of virgin contents and never sends serial setters or lock requests. HIL rejects
`otp_programming=true` for every suite. Initial physical write/lock qualification
is separate, deliberate work; after that validation succeeds, all subsequent
automated real-OTP checks remain read-only. Use emulators for mutation tests.

### H755 reliability and A/B HIL

The H755 suite covers the M7 console and sleeping M4, with networking, UART and
USB enabled. It has its own A/B tests and does not run H563 OTP tests. For a
standalone build, set up
`build/net-h755` with `board=h755`, `bootloader=false`, `networking=true`,
`uart_console=true`, `usb_console=true`, and `tcp_console=true`. Set
`probe_serial` to the H755 ST-LINK serial, and copy
[`h755.toml.example`](../tests/hil/h755.toml.example) to `build/hil-h755.toml`.
Fill in that board's stable serial paths and toolchain path. The fixture verifies
that the Meson board/options and probe selection match its configuration.

For A/B coverage, configure `build/boot-h755` with the same options except
`bootloader=true`; set `build = "build/boot-h755"` and `bootloader = true` in the
local HIL TOML. The suite then provisions `factory.hex`, including the fixed M4,
and records the bootloader, both M7 ELFs, package, and factory image in its
artifact manifest. A/B cases skip for a standalone configuration.

Run a separate, explicitly selected OpenOCD server (replace the serial):

```sh
~/install/stmicro/openocd/bin/openocd \
  -f interface/stlink-dap.cfg -c 'transport select dapdirect_swd' \
  -c 'adapter serial <H755-ST-LINK-serial>' \
  -c 'set DUAL_BANK 1; set DUAL_CORE 1' -f target/stm32h7x.cfg \
  -c 'gdb_port 3335' -c 'tcl_port 6668' -c 'telnet_port 4446' \
  -c 'adapter speed 1800' \
  -c 'reset_config srst_only srst_nogate connect_assert_srst'

build/hil-venv/bin/python -B -m pytest --hil \
  --hil-config build/hil-h755.toml tests/hil/h755 -q
```

GDB ports 3335/3336 address M7/M4. The suite uses M7 port 3335 and Tcl port 6668;
the H563 server can remain connected on its separate ports. HIL sessions remain
serialized by the shared board lock. Every H755 test erases main flash and
programs/verifies both core images, then clears retained fault RAM. It never
programs OTP or changes option bytes. The artifact manifest includes both ELFs.

Tests cover UART/USB/TCP commands, hardware/software/incremental CRC agreement,
large-packet ping, reconnect/reset, UART bursts, basic and floating-point fault
frames, configurable exceptions, retained health failures, startup failure/hang,
and interrupt-masked watchdog reset. H755 IWDG1 has no early-warning interrupt:
an arbitrary hang resets without a captured frame; a cooperative health failure
can still be recorded before feeding stops.

The A/B cases exercise confirmed A/B startup, bidirectional OTA with concurrent
timers, trial-watchdog rollback, corrupt-confirmed fallback, both-images-invalid
reset/factory recovery, and a bootloader hang. They seed a full metadata sector
in each direction to force runtime checkpoint/reclamation, and compare both
fixed-image sectors before and after OTA. `ota-timing.log` and
`journal-timing.log` under each test's artifact directory record host-observed
command round-trip times; these include network/host overhead and are not direct
scheduler latency measurements. The watchdog still enforces task progress.

Physical power cuts, actual flash ECC injection, and cache-enabled flash-operation
qualification remain separate work; simulated torn-write tests do not replace
those hardware checks.

### Heap and stack checks

Both board suites include `test_memory.py`. It checks zero `_sbrk` requests at
startup and after UART/USB/TCP command/statistics traffic and Ethernet load,
then verifies explicit heap denial. OTA tests also record the uploading image's
watermark/counters before reboot and both destination slots after startup. Artifacts include `*-memory.json` and
raw `*-stack.bin` dumps. See [STM32 memory](memory.md) for the policy and limits
of binary inspection and watermark measurements.
