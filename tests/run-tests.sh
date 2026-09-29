#!/bin/sh
# Run the host-side unit tests. Works on Linux/macOS via the
# windows.h shim in tests/shim, or in MSYS2 with plain gcc.
#
# Two suites, both linking real production code:
#   gamma_test.c   -> src/gamma.c   (ramp maths)
#   values_test.c  -> src/values.c  (slider conversion + value formatting)
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}
mkdir -p build

"$CC" -std=c99 -Wall -Wextra \
    -Itests/shim -Isrc \
    tests/gamma_test.c src/gamma.c \
    -o build/gamma_test -lm

"$CC" -std=c99 -Wall -Wextra \
    -Itests/shim -Isrc \
    tests/values_test.c src/values.c src/gamma.c \
    -o build/values_test -lm

./build/gamma_test
./build/values_test
