#!/bin/sh
# regenerate, build and compare recompiled vs interpreter per-frame state hashes
# usage: tools/lockstep.sh FRAMES   (env RANDOM_PLAY etc. passed through)
cd "$(dirname "$0")/.."
N=${1:-3000}
python3 -I tools/recomp.py rom_main.bin gen --prefix gunlock --cpu 68ec020 coverage/landing.txt | head -1
./build_test.sh >/dev/null 2>&1 || { echo build failed; exit 1; }
[ -x build/gltest_interp ] || ./build_test.sh interp >/dev/null 2>&1
build/gltest_interp "$ROMSET" $N interp "" 0 /tmp/claude-0/h_interp.txt >/dev/null 2>&1
build/gltest "$ROMSET" $N recomp "" 0 /tmp/claude-0/h_recomp.txt >/dev/null 2>&1
if cmp -s /tmp/claude-0/h_interp.txt /tmp/claude-0/h_recomp.txt; then echo "IDENTICAL over $N frames"; else diff /tmp/claude-0/h_interp.txt /tmp/claude-0/h_recomp.txt | head -3; fi
