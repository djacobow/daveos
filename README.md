# DaveOS

A C++20 cooperative scheduler for embedded applications, with fixed storage,
run-to-completion callbacks, and optional logging, commands, and networking.
Host and fake-time platforms support development without hardware. The Nucleo
H563 and H755 ports share reusable UART, USB CDC, and TCP console components.

## Learn in four steps

Each step builds on the same objects: module, platform, Application, subscriber,
and command source. Stop at the level your application needs.

1. [Hello and a periodic task](docs/01-hello.md): one module, a platform, and Application.
2. [Logging and commands](docs/02-logging-and-commands.md): add subscribers and command sources independently.
3. [A hardware console](docs/03-hardware-console.md): use a device starter and the configured Nucleo support.
4. [Custom components](docs/04-custom-components.md): add your own transport, board, or application wiring.

## Build and try

Install Meson, Ninja, a C++20 compiler, Python 3, and uv, then run from this directory:

```sh
export UV_CACHE_DIR="$PWD/build/uv-cache"
uv venv build/venv
source build/venv/bin/activate
uv pip install meson ninja -r requirements-test.txt
meson setup build/host --native-file meson/clang.ini
meson compile -C build/host
meson test -C build/host --print-errorlogs
./build/host/examples/hello/hello-host
./build/host/examples/console/console-host
```

The console accepts `help`, `console echo "hello world"`, and `console exit`.
Build outputs stay under the ignored `build/` directory.

## References and starters

- [Testing: portable checks and H563/H755 HIL](docs/testing.md)
- [Application API guide](docs/application-guide.md)
- [Build, board setup, programming, and API reference](docs/reference.md)
- [Host/fake application starter](starters/application/README.md)
- [STM32 device starter](starters/stm32/README.md)
- [Concept and core specification](PROJECT.md)
- [Utilities and portable services specification](docs/spec/services.md)
- [Platform specification](docs/spec/platforms.md)
- [STM32 integration and support matrix](docs/spec/stm32.md)
- [Outstanding work and hardware validation](TODO.md)

The STM32 starter reuses our CubeMX-generated startup and peripheral support,
not ST's board BSP. HAL, USB middleware, and lwIP remain pinned submodules.
