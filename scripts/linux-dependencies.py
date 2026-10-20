#!/usr/bin/env python3
"""Resolve the native dependency closure and retain Debian/Ubuntu notices."""
import pathlib
import re
import shutil
import subprocess
import sys

binary = pathlib.Path(sys.argv[1]).resolve()
license_dir = pathlib.Path(sys.argv[2])
license_dir.mkdir(parents=True, exist_ok=True)
allowed = re.compile(r"^lib(?:X11|Xtst|Xi|Xext|xcb|Xau|Xdmcp|bsd|md|xkbcommon|gcc_s)\.so(?:\.|$)")
host = re.compile(r"^lib(?:c|m|pthread|dl|rt|resolv|util)\.so(?:\.|$)")
text = subprocess.check_output(["ldd", str(binary)], text=True)
if "not found" in text:
    raise SystemExit("Unresolved native library dependency:\n" + text)
paths = []
for name, path in re.findall(r"(\S+)\s+=>\s+(/\S+)\s+\(", text):
    if host.match(name):
        continue
    if not allowed.match(name):
        raise SystemExit(f"Unreviewed native dependency {name}; add its deployment and license handling")
    lib = pathlib.Path(path)
    candidates = [lib, lib.resolve()]
    if str(lib).startswith("/lib/"):
        candidates.append(pathlib.Path("/usr" + str(lib)))
    elif str(lib).startswith("/usr/lib/"):
        candidates.append(pathlib.Path(str(lib)[4:]))
    package = None
    for candidate in candidates:
        result = subprocess.run(["dpkg-query", "-S", str(candidate)], capture_output=True, text=True)
        if result.returncode == 0:
            package = result.stdout.splitlines()[0].rsplit(": ", 1)[0]
            break
    if not package:
        raise SystemExit(f"Cannot find package license for {path}; build release on Debian/Ubuntu")
    name_without_arch = package.split(":")[0]
    notice = pathlib.Path("/usr/share/doc") / name_without_arch / "copyright"
    if not notice.is_file():
        raise SystemExit(f"Missing dependency copyright file: {notice}")
    shutil.copyfile(notice, license_dir / (name_without_arch + "-copyright.txt"))
    paths.append(str(lib))
common = pathlib.Path("/usr/share/common-licenses")
if common.is_dir():
    shutil.copytree(common, license_dir / "common-licenses", dirs_exist_ok=True)
for path in sorted(set(paths)):
    print(path)
