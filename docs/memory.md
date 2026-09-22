# STM32 memory and allocation

Each image gives its main stack all remaining space in its contiguous RAM
region, above static data. `_sstack` is the aligned end of static allocations;
`_estack` is the region's top. `_Min_Stack_Size` is a link-time minimum-headroom
check, not a fixed stack allocation. The linker reports static RAM usage without
adding that minimum to the total. Bootloader and application have separate
layouts and lifetimes; H755 M4 has its own RAM and stack.

H563 uses its main SRAM region; H755 M7 uses DTCM. DMA buffers remain in the
appropriate separate SRAM regions. H755 also places lwIP's fixed packet pools
(`.bss.memp_memory_*`) in a dedicated `.lwip_bss` section in AXI SRAM. Startup
zeros that section before constructors on every boot. This preserves DTCM
stack headroom when SD/FatFs is enabled; the pools remain fixed storage, not a
runtime heap. Preserve both the linker section and startup clearing loop when
regenerating CubeMX files. One stack cannot span disjoint RAM banks.
The retained fault record and emergency exception stack remain separately
reserved below ordinary RAM. H563's MSPLIM guards `_sstack`; Cortex-M7/M4 do
not provide that guard.

The CubeMX heap setting is zero. Meson excludes generated `sysmem.c` and links
`platform/stm32/memory/no_heap.c` for board applications and bootloaders. Its
`_sbrk` always returns failure with `ENOMEM`, including for a zero-size query.
There is no growing runtime heap. Debugger-visible `daveos_heap_attempts` and
`daveos_heap_last_request` record rejected calls without logging recursively.
These symbols can be garbage-collected in an image that never references them,
such as the sleeping M4.

## What the binary audit found

The earlier H755 full console had linked libc allocation and C++ exception
support despite compiling application code with exceptions disabled. A live
`board stats` call grew its heap by **744 bytes**: the average column used
floating-point `printf`. Runtime `string_view::substr` checks and libstdc++'s
assertion reporter also pulled in exception support, including a constructor
that attempted to allocate an emergency exception pool.

The console now uses nonthrowing view operations after checking bounds.
STM32 compilation disables libstdc++'s optional assertion reporter in libraries
as well as application sources; DaveOS's explicit validation and the existing
C-assert policy remain unchanged. Statistics print a one-decimal average using
integer arithmetic, rounded half up, with the same 32-bit display cap as other
counters. The underlying counters and `average()` accessor retain their precision.

**Linked allocation functions are not proof that allocation happens.** Full
newlib still includes possible allocating paths within formatted I/O, including
floating-point conversion. Conversely, `snprintf` writing to a caller-owned
buffer does not guarantee allocation-free execution. Do not assume floating-point,
locale/wide-character, or buffered stream formatting works under the no-heap
policy. Built-in STM32 log formats avoid those paths; application formats need
the same audit. A general allocation-free formatter remains separate work.
The networking module uses statically reserved lwIP pools, not the libc heap.

## Build reports

ARM board builds automatically produce reports under `tools/memory/` in their
build directory. To refresh them explicitly:

```sh
meson compile -C build/boot-h755 memory-report
```

There is one JSON report per executable: standalone/A/B application, bootloader,
and sleeping M4 as applicable. Reports include stack bounds and capacity,
minimum headroom, heap reservation, linked allocation symbols, and direct calls
to allocation functions. `tools/memory_report.py` can also inspect a separately
built consumer ELF with `--elf`, `--output`, and optional `--nm`/`--objdump` paths.
Custom cross files must identify the matching `nm` and `objdump` tools.

This is a static audit, not a proof about indirect calls or third-party private
allocators. The no-heap `_sbrk` implementation and runtime checks provide the
complementary evidence. Bootloader and application allocation counters have
independent lifetimes; one image's zero counter does not audit the other.

## Stack and hardware checks

Each reset handler calls `daveos_stack_paint` immediately after setting MSP (and
MSPLIM on H563), before stack-using initialization calls or constructors. It
fills only the linked stack with `0xa5a5a5a5`, using no stack itself. Preserve
this startup call, the linker symbols/minimum assertion, and the zero heap
setting when regenerating CubeMX files. H755 standalone M7 startup also allows
M4 time to paint its larger SRAM stack before entering stop; preserve the
extended, bounded dual-core handshake budget.

The [HIL suites](testing.md) check startup and UART/USB/TCP statistics, commands,
timers and network traffic with zero heap requests, then deliberately verify
that `_sbrk` rejects a request. Bidirectional OTA checks capture the stack and
heap counters in the uploading image before reboot and again after each
destination boots. Per-test `*-memory.json` and
`*-stack.bin` artifacts retain the results.

Measured on 2026-09-21 on the connected Nucleo boards with the full A/B console, UART/USB/TCP
commands, 1,400-byte pings, and updates in both directions:

| Image | Available stack (bytes) | Largest observed overwrite (bytes) |
| --- | ---: | ---: |
| H563 application | 501,016 | 2,664 |
| H563 bootloader | 652,344 | 2,840 |
| H755 application | 17,200 | 3,520 |
| H755 bootloader | 128,056 | 3,520 |

These runs recorded zero heap requests in both application slots and both
bootloaders. Separate H755 standalone checks observed 2,424 of 36,880 stack
bytes, and five consecutive software-reset startups passed after extending the
M4 handshake wait. The sleeping M4 has 294,864 stack bytes and no linked
allocator; its stack watermark was not measured on hardware.

A watermark measures overwritten memory. It can miss stack-pointer excursions
that leave bytes untouched and does not establish a worst-case bound for all
future inputs or interrupts. Measure representative loads and retain margin;
use the linker minimum to reject clearly undersized builds. Tests that inject
faults may also use scratch RAM, so their watermarks need separate interpretation.

The initial H755 SD/FatFs plus network-console build used 84,952 bytes of ordinary
DTCM statics/alignment, 2,048 bytes for retained faults/emergency stack, and
left 44,072 bytes for the M7 stack. AXI SRAM holds 44,544 bytes of lwIP pools,
16,384 bytes of UART DMA buffers and 31,648 bytes of Ethernet storage. The
read-only filesystem HIL workload observed 3,480 stack bytes and zero heap
attempts. These measurements apply to this configuration, not every feature
combination or a worst-case execution path.

The subsequent H755 create/readback/remove HIL cycle observed 4,064 stack
bytes out of the same 44,072-byte region and zero heap attempts. This is
higher than the read-only workload above, but still a measured workload
rather than a worst-case bound.

After incremental SD response reads replaced the fixed capture window, the
H755 DMA configuration leaves 56,360 bytes for the DTCM stack (72,664 bytes
of ordinary statics/alignment). Two aligned 512-byte SPI DMA staging buffers
add 1,024 bytes in AXI SRAM, keeping caller buffers valid even in DTCM.
Read-only filesystem HIL observed 3,328 stack bytes; create/readback/remove
observed 4,024. Both recorded zero heap attempts. These are measured workloads,
not worst-case bounds. Broader bulk-data relocation remains a TODO.
