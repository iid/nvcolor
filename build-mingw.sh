#!/bin/sh
# Build nvcolor.exe with MinGW-w64 (Git Bash / MSYS2).
set -e

cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
command -v "$CC" >/dev/null 2>&1 || CC=gcc

# MSYS2 names the cross tools without the triple prefix.
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
command -v "$WINDRES" >/dev/null 2>&1 || WINDRES=windres

mkdir -p build

# Safety net: every NVAPI id and struct size is pinned by a compile-time
# assertion. If this fails, the build stops here.
"$CC" -std=c99 -Wall -Wextra -c -Isrc verify_structs.c -o build/verify_structs.o
echo "verify_structs.c: constants OK"

# Resources. windres takes one script at a time, so compile the version
# resource and the manifest resource separately and link both objects.
"$WINDRES" -I src src/nvcolor.rc      -O coff -o build/nvcolor_res.o
"$WINDRES" -I src src/nvcolor_res.rc -O coff -o build/nvcolor_man.o

"$CC" -std=c99 -Wall -Wextra -O2 -municode -mwindows -static \
    -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DUNICODE -D_UNICODE -DNOMINMAX \
    -Isrc \
    src/nvcolor.c src/nvapi_min.c src/gamma.c src/state.c src/values.c build/nvcolor_res.o build/nvcolor_man.o \
    -o build/nvcolor.exe \
    -luser32 -lgdi32 -lm

echo "Built build/nvcolor.exe"
