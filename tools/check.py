#!/usr/bin/env python3
"""Project-local format and lint entry points; all caches stay under build/."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ("storage/fatfs", "net/lwip", "platform/stm32/lan8742", "platform/stm32/STM32_USB_Device_Library", "platform/stm32h5/STM32CubeH5", "platform/stm32h7/STM32CubeH7",
             "platform/stm32/nucleo/h563/Core", "platform/stm32/nucleo/h563/Drivers",
             "platform/stm32/nucleo/h755/CM7/Core",
             "platform/stm32/nucleo/h755/CM4/Core",
             "platform/stm32/nucleo/h755/Common",
             "platform/stm32/nucleo/h755/Drivers")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("check", choices=("format", "format-check", "lint"))
    args = parser.parse_args()
    sources = sorted(
        str(path) for directory in ("core", "hal", "drivers", "storage", "util", "boot", "update", "otp", "watchdog", "console", "net", "platform", "examples", "starters", "tests")
        for path in (ROOT / directory).rglob("*") if path.suffix in (".h", ".hpp", ".c", ".cpp")
        and not any(path.is_relative_to(ROOT / folder) for folder in GENERATED)
    )
    if args.check.startswith("format"):
        executable = shutil.which("clang-format-15") or shutil.which("clang-format")
        if not executable:
            sys.exit("Install clang-format (version 15 recommended).")
        options = ["-i"] if args.check == "format" else ["--dry-run", "--Werror"]
        subprocess.run([executable, "--style=file", *options, *sources], cwd=ROOT, check=True)
    else:
        executable = shutil.which("cppcheck")
        if not executable:
            sys.exit("Install cppcheck to run lint.")
        cache = ROOT / "build" / "cppcheck"
        cache.mkdir(parents=True, exist_ok=True)
        command = [
            executable, "--std=c++20", "--enable=warning,performance,portability",
            "--error-exitcode=1", "--inline-suppr", "--template=gcc",
            # CRTP deliberately replaces inherited defaults without virtual methods.
            "--suppress=duplInheritedMember",
            *[f"-i{folder}" for folder in GENERATED],
            f"--cppcheck-build-dir={cache}", "-I.",
        ]
        subprocess.run(command + [name for name in
            ("core", "hal", "drivers", "storage", "util", "boot", "update", "otp", "watchdog", "console", "net", "platform", "examples", "starters")
            if (ROOT / name).is_dir()], cwd=ROOT, check=True)
        # Directory traversal omits unreferenced header-only components. Check
        # HAL headers explicitly, including C++ code using the .h convention.
        hal_headers = sorted(str(path) for path in (ROOT / "hal").rglob("*")
                             if path.suffix in (".h", ".hpp"))
        hal_headers += [str(path) for path in (ROOT / "drivers").rglob("*")
                        if path.suffix in (".h", ".hpp")]
        hal_headers += [str(ROOT / name) for name in (
            "platform/fake/bus.hpp", "platform/stm32/bus/driver.h",
            "platform/stm32/bus/spi_dma.h", "platform/stm32h7/spi_dma.h",
            "platform/stm32h5/spi_dma.h",
            "storage/module.hpp", "storage/read_file.h", "update/file.h", "storage/sd/initializer.h", "storage/sd/read.h", "storage/sd/transport.h",
            "storage/sd/session.h", "watchdog/health.hpp")]
        subprocess.run(command + ["--language=c++", *hal_headers], cwd=ROOT, check=True)



if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
