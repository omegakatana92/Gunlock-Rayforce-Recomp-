#!/usr/bin/env python3
"""extract and interleave the 68EC020 program ROMs (ROM_LOAD32_BYTE d66-18/19/21/24) from gunlock.zip or a folder"""
import sys, os, zipfile, zlib
WANT = [('d66-18.ic24', 0x8418513e), ('d66-19.ic26', 0x95731473), ('d66-21.ic37', 0xbd0d60f2), ('d66-24.ic35', 0x97816378)]
src = sys.argv[1]
def get(name, crc):
    if os.path.isdir(src):
        return open(os.path.join(src, name), 'rb').read()
    z = zipfile.ZipFile(src)
    for i in z.infolist():
        if i.CRC == crc: return z.read(i)
    return z.read(name)
parts = []
for n, c in WANT:
    d = get(n, c)
    if zlib.crc32(d) != c: sys.exit('%s: bad CRC (not the MAME gunlock set)' % n)
    parts.append(d)
out = bytearray(len(parts[0]) * 4)
for k, p in enumerate(parts): out[k::4] = p
open(sys.argv[2], 'wb').write(out)
