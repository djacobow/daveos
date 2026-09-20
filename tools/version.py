#!/usr/bin/env python3
"""Emit a reproducible version stamp; only rewrite when its contents change."""
import argparse
import json
from pathlib import Path
import subprocess


def u32(text):
    value = int(text)
    if not 0 <= value <= 0xffffffff:
        raise argparse.ArgumentTypeError("expected a uint32 value")
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--component", choices=("application", "bootloader"), default="application")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--major", type=u32, required=True)
    parser.add_argument("--minor", type=u32, required=True)
    parser.add_argument("--build", type=u32, default=0xffffffff)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        commit = subprocess.check_output(
            ["git", "-C", str(args.root), "rev-parse", "HEAD"], text=True,
            stderr=subprocess.DEVNULL).strip()
        dirty = bool(subprocess.check_output(
            ["git", "-C", str(args.root), "status", "--porcelain", "--untracked-files=normal"],
            text=True, stderr=subprocess.DEVNULL).strip())
    except subprocess.CalledProcessError:
        commit, dirty = "unknown", False
    content = ("#pragma once\n\n#include \"util/version.h\"\n\n"
               "namespace daveos::build {\n"
               f"inline constexpr util::Version k{args.component.title()}Version{{"
               f"{args.major}u, {args.minor}u, {args.build}u, "
               f"{json.dumps(commit)}, {'true' if dirty else 'false'}"
               "};\n}\n")
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)
    if args.json:
        content = json.dumps(dict(major=args.major, minor=args.minor, build=args.build,
                                  commit=commit, dirty=dirty), indent=2) + "\n"
        if not args.json.exists() or args.json.read_text() != content:
            args.json.write_text(content)


if __name__ == "__main__":
    main()
