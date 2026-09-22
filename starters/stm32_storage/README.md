# DaveOS STM32 storage starter

A second small Nucleo application, built only from DaveOS libraries. It adds an
SPI SD card and read-only FAT commands to the USART3 console:

- `card.hpp`: a module that owns `stm32::SdCard` (the Nucleo SD wiring driving
  the reusable `storage::sd::Session`), probes the card when dispatch starts,
  and offers `card probe` to reinitialize it.
- `appmain.cpp`: the USART3 console, board commands, the card module and
  `storage::Module` (`fs mount`, `fs ls`, `fs read`, `fs unmount`), plus the
  SPI1 interrupt route.

It does not include anything from DaveOS's `examples/` directory. SD payloads
use interrupts, not DMA, so one interrupt handler serves both boards; the shared
console example shows the DMA variant.

Wire an SDHC/SDXC card to SPI1: H563 PA5/PG9/PB5 (SCK/MISO/MOSI), H755
PA5/PA6/PB5, and PD14 as chip select on both. Build it like the
[STM32 starter](../stm32/README.md), from a copy of this directory with DaveOS
available as `subprojects/daveos`:

```sh
export PATH="$PWD/tools/external/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
meson setup build --cross-file meson/stm32.ini -Dboard=h563
meson compile -C build
meson compile -C build flash
```

On the USART3 console (1,000,000 baud), expect `SD card ready: <size> MiB`,
then:

```text
fs mount
fs ls
fs read "some file.txt"
fs unmount
```

The subproject is configured with `features=uart,sd,fatfs`; FatFs mounts
read-only unless you pass `fs mount rw`.
