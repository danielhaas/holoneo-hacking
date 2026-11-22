#!/usr/bin/env python3
"""Split an ESP32 app image (e.g. the factory partition) into its load segments.

Usage: split_image.py app.bin outdir/
Writes outdir/seg_<loadaddr>.bin for each segment; feed the code segments to
xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=<loadaddr>.
"""
import os, struct, sys

src, out = sys.argv[1], sys.argv[2]
d = open(src, "rb").read()
assert d[0] == 0xE9, "not an ESP app image"
os.makedirs(out, exist_ok=True)
off = 24
for _ in range(d[1]):
    addr, ln = struct.unpack_from("<II", d, off)
    open(os.path.join(out, f"seg_{addr:08x}.bin"), "wb").write(d[off + 8: off + 8 + ln])
    print(f"seg {addr:#010x} len {ln}")
    off += 8 + ln
