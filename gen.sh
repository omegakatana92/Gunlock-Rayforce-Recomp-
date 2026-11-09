#!/bin/sh
# Generates the recompiled C code (gen/) from your own copy of the MAME 'gunlock' romset.
#   ./gen.sh /path/to/gunlock.zip      (or a folder holding the ROM files)
set -e
cd "$(dirname "$0")"
[ -n "$1" ] || { echo "usage: $0 /path/to/gunlock.zip"; exit 1; }
python3 tools/extract_prog.py "$1" rom_main.bin
[ -f tools/libdis.so ] || gcc -O2 -fPIC -shared -Imusashi -o tools/libdis.so tools/dislib.c musashi/m68kdasm.c
python3 -I tools/recomp.py rom_main.bin gen --prefix gunlock --cpu 68ec020 coverage/landing.txt
