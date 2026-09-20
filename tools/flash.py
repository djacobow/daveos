#!/usr/bin/env python3
"""Program built STM32 ELF images through ST-LINK, or preview the commands."""
import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def find_tool(name, override):
    if override:
        candidate = str(Path(override).expanduser())
        found = shutil.which(candidate)
        if found:
            return found
        raise ValueError(f"Cannot execute {candidate}")
    roots = {
        "openocd": ("stmicro/openocd/bin",),
        "STM32_Programmer_CLI": ("stmicro/STM32CubeProgrammer/bin", "STM32CubeProgrammer/bin"),
    }
    for directory in roots[name]:
        found = shutil.which(str(Path.home() / "install" / directory / name))
        if found:
            return found
    found = shutil.which(name)
    if found:
        return found
    raise ValueError(f"Cannot find {name}; set its Meson path option or add it to PATH")


def tcl_word(value):
    # Quote one Tcl word, including paths with spaces, braces, or substitutions.
    return '"' + ''.join('\\' + c if c in '\\"$[]' else c for c in value) + '"'


def command(backend, executable, family, images, serial, erase_all=False):
    if backend == "cubeprogrammer":
        args = [executable, "-c", "port=SWD", "mode=UR", "reset=HWrst", "ap=0"]
        if serial:
            args.append(f"sn={serial}")
        if erase_all:
            args += ["-e", "all"]
        for image in images:
            args += ["-d", str(image), "-v"]
        return args + ["-rst"]
    args = [executable, "-f", "interface/stlink-dap.cfg",
            "-c", "transport select dapdirect_swd"]
    if serial:
        args += ["-c", "adapter serial " + tcl_word(serial)]
    if family == "stm32h7":
        args += ["-c", "set DUAL_BANK 1; set DUAL_CORE 1"]
    args += ["-f", f"target/{family}x.cfg", "-c", "adapter speed 1800",
             "-c", "reset_config srst_only srst_nogate connect_assert_srst",
             "-c", "init; reset halt"]
    if erase_all:
        if family != "stm32h5":
            raise ValueError("Factory mass erase is currently supported only for H563")
        args += ["-c", "stm32h5x mass_erase 0"]
    for image in images:
        args += ["-c", "flash write_image erase " + tcl_word(str(image)),
                 "-c", "verify_image " + tcl_word(str(image))]
    return args + ["-c", "reset run; shutdown"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("cubeprogrammer", "openocd", "plan"), required=True)
    parser.add_argument("--family", choices=("stm32h5", "stm32h7"), required=True)
    parser.add_argument("--openocd", default="")
    parser.add_argument("--cubeprogrammer", default="")
    parser.add_argument("--serial", default="")
    parser.add_argument("--erase-all", action="store_true",
                        help="Erase all main flash before factory programming (preserves OTP)")
    parser.add_argument("images", nargs="+", type=Path)
    args = parser.parse_args()
    images = [image.resolve() for image in args.images]
    for image in images:
        if not image.is_file():
            parser.error(f"Image does not exist: {image}")
    backends = ("cubeprogrammer", "openocd") if args.backend == "plan" else (args.backend,)
    for backend in backends:
        name = "openocd" if backend == "openocd" else "STM32_Programmer_CLI"
        try:
            executable = find_tool(name, getattr(args, backend))
        except ValueError as error:
            if args.backend != "plan":
                parser.error(str(error))
            print(str(error), file=sys.stderr)
            # Preview is useful on CI and hosts without programming tools too.
            executable = str(Path(getattr(args, backend)).expanduser()) if getattr(args, backend) else name
        invocation = command(backend, executable, args.family, images, args.serial, args.erase_all)
        print(shlex.join(invocation), flush=True)
        if args.backend != "plan":
            subprocess.run(invocation, check=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
