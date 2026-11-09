#!/bin/sh
# Builds release packages into dist/ (no ROM data is included):
#   Gunlock-Recomp-Windows-x64.zip   gunlock.exe (SDL2 static) + README + licences
#   Gunlock-Recomp-x86_64.AppImage   Steam Deck / Linux (glibc 2.17 baseline; bundled SDL2 used only if the system has none)
#   Gunlock-Recomp-Linux-x86_64.tar.gz  the same Linux binary without AppImage packaging
# needs: mingw-w64, python3 -m ziglang, appimagetool (APPIMAGETOOL=path), desktop-file-utils
set -e
cd "$(dirname "$0")"
VER=${VER:-0.1.0}
./build_frontend.sh windows
./build_frontend.sh deck
rm -rf dist; mkdir -p dist/stage
# ---- docs common
mkdir -p dist/stage/licences
cp README.md dist/stage/README.md
cp musashi/readme.txt dist/stage/licences/Musashi.txt
cp assets/fonts/OFL-*.txt dist/stage/licences/
cp docs/THIRD_PARTY.md dist/stage/licences/
# ---- windows
W=dist/stage/Gunlock-Recomp-Windows-x64; mkdir -p $W
cp build/fe-windows/gunlock.exe $W/; cp -r dist/stage/README.md dist/stage/licences $W/
(cd dist/stage && zip -qr ../Gunlock-Recomp-Windows-x64.zip Gunlock-Recomp-Windows-x64)
# ---- linux tarball
L=dist/stage/Gunlock-Recomp-Linux-x86_64; mkdir -p $L
cp build/fe-deck/gunlock $L/; cp -r dist/stage/README.md dist/stage/licences $L/
(cd dist/stage && tar czf ../Gunlock-Recomp-Linux-x86_64.tar.gz Gunlock-Recomp-Linux-x86_64)
# ---- AppImage
A=dist/stage/AppDir; mkdir -p $A/usr/bin $A/usr/lib $A/usr/share/doc/gunlock
cp build/fe-deck/gunlock $A/usr/bin/
cp third_party/sdl2linux/lib/libSDL2-2.0.so.0 $A/usr/lib/
cp -r dist/stage/README.md dist/stage/licences $A/usr/share/doc/gunlock/
cp win/gunlock.png $A/gunlock.png
cat > $A/gunlock.desktop <<'D'
[Desktop Entry]
Type=Application
Name=Gunlock
Comment=Gunlock (Taito, 1993) native recompilation
Exec=gunlock
Icon=gunlock
Categories=Game;ArcadeGame;
D
cat > $A/AppRun <<'R'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
# prefer the system SDL2 (Steam Deck / Steam runtime builds handle gamescope + controllers best)
if ! (ldconfig -p 2>/dev/null | grep -q 'libSDL2-2.0.so.0'); then export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"; fi
exec "$HERE/usr/bin/gunlock" "$@"
R
chmod +x $A/AppRun
ARCH=x86_64 ${APPIMAGETOOL:-appimagetool} --no-appstream $A dist/Gunlock-Recomp-x86_64.AppImage >/dev/null 2>&1
rm -rf dist/stage
(cd dist && sha256sum * > SHA256SUMS.txt && cat SHA256SUMS.txt)
