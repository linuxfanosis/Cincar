#!/usr/bin/env bash
set -u
BUILD=${1:-build}
TMP=$(mktemp -d)
fail=0

check() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1 (expected '$3', got '$2')"; fail=1; fi
}

gcc -nostdlib -static -no-pie tests/hello.S -o "$TMP/hello"
out=$("$BUILD/runelf" "$TMP/hello"); code=$?
check "hello output" "$out" "hello, loader"
check "hello exit code" "$code" "0"

gcc -nostdlib -static -no-pie tests/exit7.S -o "$TMP/exit7"
"$BUILD/runelf" "$TMP/exit7"; code=$?
check "exit code propagates" "$code" "7"

echo "not an elf" > "$TMP/bad"
err=$("$BUILD/runelf" "$TMP/bad" 2>&1); code=$?
check "bad file rejected" "$code" "1"
check "bad file message" "$err" "error: file too small for an ELF64 header"

mine=$("$BUILD/elfinfo" "$TMP/hello" | head -1 | sed -E 's/.*entry (0x[0-9a-f]+).*/\1/')
ref=$(readelf -h "$TMP/hello" | awk '/Entry point/{print $4}')
check "entry matches readelf" "$mine" "$ref"

exit $fail
