#!/bin/sh
# Builds the RetroFront + Gunlock executable. Run ./gen.sh <gunlock.zip> first (generates gen/ from YOUR ROM).
#   ./build_frontend.sh linux     -> build/fe-linux/gunlock        (native gcc, for testing)
#   ./build_frontend.sh deck      -> build/fe-deck/gunlock         (zig cc, glibc 2.17 baseline, system libSDL2: Steam Deck / any distro)
#   ./build_frontend.sh windows   -> build/fe-windows/gunlock.exe  (mingw-w64, SDL2 static, single .exe)
set -e
cd "$(dirname "$0")"
[ -f gen/sources.txt ] || { echo "gen/ is missing: run ./gen.sh /path/to/gunlock.zip first"; exit 1; }
T=${1:-linux}
case $T in
  linux)   CC=gcc; CXX=g++; SDLINC="$(sdl2-config --cflags)"; LIBS="-lSDL2 -lm -lpthread"; OUT=build/fe-linux/gunlock; EXTRA="" ;;
  deck)    CC="python3 -m ziglang cc -target x86_64-linux-gnu.2.17"; CXX="python3 -m ziglang c++ -target x86_64-linux-gnu.2.17";
           SDLINC="-Ithird_party/sdl2linux/include/SDL2 -D_REENTRANT"; LIBS="-Lthird_party/sdl2linux/lib -lSDL2 -lm -Wl,--allow-shlib-undefined -s"; OUT=build/fe-deck/gunlock; EXTRA="" ;;
  windows) CC=x86_64-w64-mingw32-gcc; CXX=x86_64-w64-mingw32-g++; W=third_party/sdl2win/x86_64-w64-mingw32
           SDLINC="-I$W/include/SDL2 -Dmain=SDL_main"; OUT=build/fe-windows/gunlock.exe; EXTRA="win/gunlock.res.o"
           [ -f win/gunlock.res.o ] || x86_64-w64-mingw32-windres win/gunlock.rc -O coff -o win/gunlock.res.o
           LIBS="-L$W/lib -static -lmingw32 -lSDL2main -lSDL2 -mwindows -lm -ldinput8 -ldxguid -ldxerr8 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lshell32 -lsetupapi -lversion -luuid -ladvapi32 -s" ;;
  *) echo "usage: $0 linux|deck|windows"; exit 1 ;;
esac
O=build/obj-$T; mkdir -p $O $(dirname $OUT)
CFLAGS="-O2 -w -Isrc -Imusashi -Ithird_party/miniz"
CXXFLAGS="-O2 -std=c++17 -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Iretrofront/include -Iretrofront/src -Ithird_party/json -Ithird_party/stb -Ithird_party/miniz -Isrc $SDLINC"
CSRC="src/hw.c src/cpu.c src/video.c src/sound.c src/rc_run.c $(sed 's#^#gen/#' gen/sources.txt | tr '\n' ' ') musashi/m68kcpu.c musashi/m68kops.c musashi/m68kdasm.c musashi/softfloat/softfloat.c third_party/miniz/miniz.c"
XSRC="retrofront/src/platform.cpp retrofront/src/gfx.cpp retrofront/src/display.cpp retrofront/src/io.cpp retrofront/src/saves.cpp retrofront/src/menu.cpp retrofront/src/frontend.cpp retrofront/src/fonts_embed.cpp games/gunlock/adapter_gunlock.cpp games/gunlock/profile_embed.cpp"
OBJS=""
for f in $CSRC; do o=$O/$(echo $f | tr '/' '_').o; OBJS="$OBJS $o"; if [ ! -f $o ] || [ $f -nt $o ] || [ src/core.h -nt $o ]; then echo "  CC  $f"; $CC $CFLAGS $SDLINC -c $f -o $o & fi; done; wait
for f in $XSRC; do o=$O/$(echo $f | tr '/' '_').o; OBJS="$OBJS $o"; if [ ! -f $o ] || [ $f -nt $o ] || [ src/core.h -nt $o ]; then echo "  CXX $f"; $CXX $CXXFLAGS -c $f -o $o & fi; done; wait
echo "  LINK $OUT"
$CXX -o $OUT $OBJS $EXTRA $LIBS
