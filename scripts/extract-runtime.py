#!/usr/bin/env python3
"""Reuse a verified ELF64 AppImage runtime, clearing payload-specific metadata."""
import pathlib
import struct
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
end = int(sys.argv[2])
if not 1024 < end <= len(source):
    raise SystemExit('Invalid runtime byte count')
runtime = bytearray(source[:end])
if runtime[:6] != b'\x7fELF\x02\x01':
    raise SystemExit('Expected a little-endian ELF64 AppImage runtime')
section_offset = struct.unpack_from('<Q', runtime, 0x28)[0]
entry_size, section_count, names_index = struct.unpack_from('<HHH', runtime, 0x3A)
if entry_size != 64 or not 0 < names_index < section_count < 1024:
    raise SystemExit('Unexpected ELF section table')
if section_offset + entry_size * section_count > end:
    raise SystemExit('ELF section table exceeds runtime length')
sections = [struct.unpack_from('<IIQQQQIIQQ', runtime, section_offset + i * entry_size)
            for i in range(section_count)]
names_header = sections[names_index]
names = runtime[names_header[4]:names_header[4] + names_header[5]]
clear = {b'.upd_info', b'.digest_md5', b'.sha256_sig', b'.sig_key'}
seen = set()
for section in sections:
    name = bytes(names[section[0]:]).split(b'\0', 1)[0]
    if name in clear:
        start, length = section[4], section[5]
        if start + length > end:
            raise SystemExit('AppImage metadata exceeds runtime length')
        runtime[start:start + length] = b'\0' * length
        seen.add(name)
if seen != clear:
    raise SystemExit('Missing expected AppImage metadata sections')
pathlib.Path(sys.argv[3]).write_bytes(runtime)
