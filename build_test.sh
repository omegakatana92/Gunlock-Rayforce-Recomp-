#!/bin/sh
# headless test harness build: ./build_test.sh [interp]  (interp = no generated code, Musashi only)
set -e
cd "$(dirname "$0")"
O=build/obj-test; mkdir -p $O build
CFLAGS="-O2 -g -w -Isrc -Imusashi -Ithird_party/miniz"
if [ "$1" = "interp" ]; then GEN="src/rc_stub.c"; else GEN="src/rc_run.c $(sed 's#^#gen/#' gen/sources.txt | tr '\n' ' ')"; fi
SRC="src/hw.c src/cpu.c src/video.c src/sound.c $GEN musashi/m68kcpu.c musashi/m68kops.c musashi/m68kdasm.c musashi/softfloat/softfloat.c third_party/miniz/miniz.c src/main_test.c"
OBJS=""
for f in $SRC; do o=$O/$(echo $f | tr '/' '_').o; OBJS="$OBJS $o"; if [ ! -f $o ] || [ $f -nt $o ] || [ src/core.h -nt $o ]; then gcc $CFLAGS -c $f -o $o & fi; done; wait
gcc -o build/gltest${1:+_$1} $OBJS -lm
echo built build/gltest${1:+_$1}
