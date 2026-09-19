"""Build a separate consumer project against this checkout, without downloads."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

root, work = map(Path, sys.argv[1:])
env = os.environ.copy()
# Meson may discover ccache automatically; its artifacts belong in build/ too.
env["CCACHE_DIR"] = str(work / "ccache")
source = work / "application"
shutil.copytree(root / "starters/application", source, dirs_exist_ok=True)
checkout = source / "subprojects/daveos"
if not checkout.exists():
    checkout.symlink_to(root, target_is_directory=True)
build = work / "build"
setup = ["meson", "setup", str(build), str(source), "--wrap-mode=nodownload"]
if (build / "meson-private/coredata.dat").exists():
    setup.append("--reconfigure")
for command in (setup, ["meson", "compile", "-C", str(build)],
                ["meson", "test", "-C", str(build), "--print-errorlogs"],
                [str(build / "my-app")]):
    subprocess.run(command, check=True, env=env)
