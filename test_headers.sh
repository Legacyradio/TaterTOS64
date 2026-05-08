#!/bin/bash
# test_headers.sh — Verification for Phase 2: Header Hardening
# Compiles minimal files to ensure headers are self-contained and order-independent.

CC="/home/legacyindieradio/TaterTOS64/tools/host/bin/x86_64-elf-gcc"
CFLAGS="-ffreestanding -fno-stack-protector -Isrc/include -Isrc/user/libc -c"

mkdir -p build/header_tests
rm -f build/header_tests/*.o

echo "Testing single headers..."
for h in sys/types.h sys/stat.h sys/socket.h time.h unistd.h fcntl.h poll.h signal.h netdb.h pthread.h; do
    echo "  <${h}>"
    echo "#include <${h}>" > build/header_tests/test.c
    if ! $CC $CFLAGS build/header_tests/test.c -o build/header_tests/test.o 2>/dev/null; then
        echo "    FAIL: <${h}> is not self-contained"
        $CC $CFLAGS build/header_tests/test.c -o build/header_tests/test.o
    fi
done

echo "Testing header combinations..."
COMBOS=(
    "sys/types.h sys/stat.h"
    "time.h sys/time.h"
    "sys/socket.h netinet/in.h arpa/inet.h netdb.h"
    "fcntl.h unistd.h"
)

for combo in "${COMBOS[@]}"; do
    echo "  [ ${combo} ]"
    echo "" > build/header_tests/test.c
    for h in $combo; do
        echo "#include <${h}>" >> build/header_tests/test.c
    done
    if ! $CC $CFLAGS build/header_tests/test.c -o build/header_tests/test.o 2>/dev/null; then
        echo "    FAIL: [ ${combo} ] has conflicts"
        $CC $CFLAGS build/header_tests/test.c -o build/header_tests/test.o
    fi
done

echo "Header tests complete."
