#!/usr/bin/env bash
# Compila TUI y GUI en MSYS2 UCRT64.  Uso:  bash build_msys2.sh
set -e
cd "$(dirname "$0")"
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-git mingw-w64-ucrt-x86_64-raylib
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
echo
echo "Listo:"
ls -1 build/bin/*.exe
