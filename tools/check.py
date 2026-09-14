#!/usr/bin/env python3
"""Project-local format and lint entry points; all caches stay under build/."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ("net/lwip", "platform/stm32/lan8742", "platform/stm32/STM32_USB_Device_Library", "platform/stm32h5/STM32CubeH5", "platform/stm32h7/STM32CubeH7",
             "examples/stm32h563_blinky/Core", "examples/stm32h563_blinky/Drivers",
             "examples/stm32h755_console/CM7/Core",
             "examples/stm32h755_console/CM4/Core",
             "examples/stm32h755_console/Common",
             "examples/stm32h755_console/Drivers")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("check", choices=("format", "format-check", "lint"))
    args = parser.parse_args()
    sources = sorted(
        str(path) for directory in ("core", "net", "platform", "examples", "tests")
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
        subprocess.run([
            executable, "--std=c++20", "--enable=warning,performance,portability",
            "--error-exitcode=1", "--inline-suppr", "--template=gcc",
            # CRTP deliberately replaces inherited defaults without virtual methods.
            "--suppress=duplInheritedMember",
            *[f"-i{folder}" for folder in GENERATED],
            f"--cppcheck-build-dir={cache}", "-I.", "core", "net", "platform", "examples",
        ], cwd=ROOT, check=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
