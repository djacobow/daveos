# A small STM32 application

This standalone Meson project runs one periodic Worker and the standard board
commands over USART3. `appmain.cpp` owns the platform, UART console, logger,
Application, and modules. The `daveos-nucleo` dependency supplies configured
CubeMX startup, linker settings, HAL components, and peripheral setup. It uses
our pinned HAL submodules, not ST's board BSP.

NUCLEO-H563ZI is the default. NUCLEO-H755ZI-Q is selected with `-Dboard=h755`;
its separate M4 image completes startup synchronization and sleeps immediately.

## Start with a local checkout

To develop against a local checkout, start from the DaveOS repository root
with an unused destination:

```sh
cp -a starters/stm32 /tmp/my-device
ln -s "$PWD" /tmp/my-device/subprojects/daveos
export PATH="$PWD/tools/external/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
cd /tmp/my-device
```

Install Meson, Ninja, Python 3, and an ARM C/C++20 compiler with newlib if needed.
The commands below assume `arm-none-eabi-*` tools are on PATH.
For a published checkout instead of a symlink, run `meson subprojects download`
from the copied project; the wrap revision pins the tested API commit.

Initialize the board's HAL/CMSIS submodules in the DaveOS checkout. For H563:

```sh
git -C subprojects/daveos submodule update --init platform/stm32h5/STM32CubeH5
git -C subprojects/daveos/platform/stm32h5/STM32CubeH5 submodule update --init --recursive Drivers/CMSIS/Device/ST/STM32H5xx Drivers/STM32H5xx_HAL_Driver
meson setup build/arm --cross-file meson/stm32.ini
meson compile -C build/arm
meson compile -C build/arm flash-plan
```

For H755, initialize `platform/stm32h7/STM32CubeH7`, then its
`Drivers/CMSIS/Device/ST/STM32H7xx` and `Drivers/STM32H7xx_HAL_Driver` submodules,
and configure a fresh build with:

```sh
meson setup build/h755 --cross-file meson/stm32.ini -Dboard=h755
meson compile -C build/h755
meson compile -C build/h755 flash-plan
```

For an already configured directory, change both `-Dboard` and `-Ddaveos:board`
together; the build rejects mismatches. Other subproject options may be set with
the `-Ddaveos:` prefix. This starter deliberately wires only UART; enabling USB
or networking also requires adding the desired modules and registrations to
`appmain.cpp`, as shown in the full STM32 example.

## Program and use it

`flash-plan` prints commands without contacting hardware. Use `flash-openocd`
for OpenOCD or `flash` for STM32CubeProgrammer. Configure `-Dopenocd=...`,
`-Dcubeprogrammer=...`, and `-Dprobe_serial=...` as needed. H755 flashing includes
both the application and sleeping M4 image.

Open ST-LINK's virtual serial port at **1,000,000 baud, 8N1**. You should see a
worker tick each second. Try `help`, `board led 1 on`, `board button`,
`board timer 200000`, `board stats`, and `board reset`.

The application image is `build/arm/my-device.elf`, with `.hex`, `.bin`, and `.map`
files alongside it. Board support propagates the CPU/hard-float flags and linker
script. H755's M4 artifacts live under the build's
`subprojects/daveos/platform/stm32/nucleo/h755/CM4/` directory.

Change `worker.hpp` for application behavior. For another board, replace the
`daveos-nucleo` dependency with your startup/linker/peripheral support and supply
`board_config.h`; the console library remains separately reusable. See
[the hardware guide](../../docs/03-hardware-console.md) and
[custom components](../../docs/04-custom-components.md) in the DaveOS repository.
