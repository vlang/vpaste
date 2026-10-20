#!/usr/bin/env python3
"""Retain notices from the exact V runtime used by this build."""
import pathlib
import re
import shutil
import sys

compiler = pathlib.Path(sys.argv[1]).resolve()
if not compiler.is_file():
    found = shutil.which(str(compiler)) or shutil.which(sys.argv[1])
    if not found:
        raise SystemExit("Cannot locate V compiler to collect runtime notices")
    compiler = pathlib.Path(found).resolve()
root = compiler.parent
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
license_file = root / "LICENSE"
if not license_file.is_file():
    raise SystemExit("V compiler tree lacks LICENSE; use scripts/bootstrap-v first")
shutil.copyfile(license_file, out / "V-LICENSE.txt")
# V bundles the collector as an amalgamation with distributed license notices.
# Retain every unique licensing/copyright comment in the amalgamation + headers.
comments = []
seen = set()
inputs = [root / "thirdparty/libgc/gc.c"]
inputs += sorted((root / "thirdparty/libgc/include").rglob("*.h"))
for path in inputs:
    if not path.is_file():
        raise SystemExit(f"Missing runtime source: {path}")
    for comment in re.findall(r"/\*.*?\*/", path.read_text(errors="replace"), re.S):
        if re.search(r"copyright|permission|license", comment, re.I) and comment not in seen:
            seen.add(comment)
            comments.append(comment)
(out / "BDWGC-NOTICES.txt").write_text(
    "Notices retained from the V compiler's bundled BDWGC sources.\n"
    "Upstream: https://github.com/ivmai/bdwgc\n\n" + "\n\n".join(comments) + "\n"
)
atomic = root / "thirdparty/libatomic_ops/LICENSE"
if atomic.is_file():
    shutil.copyfile(atomic, out / "LIBATOMIC_OPS-LICENSE.txt")
