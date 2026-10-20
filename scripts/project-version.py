#!/usr/bin/env python3
"""Fail release builds on inconsistent source versions or a mismatched tag."""
import os
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
mod = re.search(r"\bversion:\s*'([^']+)'", (root / 'v.mod').read_text())
core = re.search(r"\bconst version\s*=\s*'([^']+)'", (root / 'core/config.v').read_text())
if not mod or not core or mod[1] != core[1]:
    raise SystemExit('v.mod and core/config.v must declare the same version')
version = mod[1]
if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9.]+)?', version):
    raise SystemExit('Invalid project version')
ref = os.environ.get('GITHUB_REF', '')
if ref.startswith('refs/tags/') and ref != 'refs/tags/v' + version:
    raise SystemExit(f'Tag {ref} does not match source version v{version}')
print(version)
