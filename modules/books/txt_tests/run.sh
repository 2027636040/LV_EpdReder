#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
build=$(mktemp -d /tmp/txt-reader-tests.XXXXXX)
trap 'rm -f "$build/test"; rmdir "$build"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Wno-type-limits -Wno-sign-compare -g -fsanitize=address,undefined \
    -I"$root/modules/books/txt_tests/include" -I"$root/src/services" -I"$root/src/ui" \
    "$root/modules/books/txt_tests/txt_test.c" -o "$build/test"
ASAN_OPTIONS=detect_leaks=1 "$build/test"
