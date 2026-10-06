#!/usr/bin/env bash
set -u
BUILD=${1:-build}
TMP=$(mktemp -d)
fail=0

check() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1 (expected '$3', got '$2')"; fail=1; fi
}

for t in hello exit7 getpid crash escape; do
    gcc -nostdlib -static -no-pie tests/$t.S -o "$TMP/$t"
done

out=$("$BUILD/runelf" "$TMP/hello"); code=$?
check "hello output" "$out" "hello, loader"
check "hello exit code" "$code" "0"

"$BUILD/runelf" "$TMP/exit7"; code=$?
check "exit code propagates" "$code" "7"

# The guest's syscalls must go through our runtime, not the kernel.
trace=$(CINCAR_TRACE=1 "$BUILD/runelf" "$TMP/hello" 2>&1 >/dev/null)
check "write intercepted" "$(echo "$trace" | grep -c 'guest\] write(1')" "1"
check "exit intercepted" "$(echo "$trace" | grep -c 'guest\] exit(0)')" "1"

# Unimplemented syscalls fail loudly and return ENOSYS (38) to the guest.
msg=$("$BUILD/runelf" "$TMP/getpid" 2>&1); code=$?
check "unimplemented syscall -> ENOSYS" "$code" "38"
check "unimplemented syscall is named" "$(echo "$msg" | grep -c 'unimplemented guest syscall 39')" "1"

# Our runtime refuses writes to fds the guest shouldn't touch (EBADF = 9).
"$BUILD/runelf" "$TMP/escape"; code=$?
check "bad fd rejected with EBADF" "$code" "9"

# A guest crash must be reported, not take us down.
err=$("$BUILD/runelf" "$TMP/crash" 2>&1); code=$?
check "guest crash exit code" "$code" "1"
check "guest crash message" "$err" "error: guest stopped by signal 11"

echo "not an elf" > "$TMP/bad"
err=$("$BUILD/runelf" "$TMP/bad" 2>&1); code=$?
check "bad file rejected" "$code" "1"
check "bad file message" "$err" "error: file too small for an ELF64 header"

mine=$("$BUILD/elfinfo" "$TMP/hello" | head -1 | sed -E 's/.*entry (0x[0-9a-f]+).*/\1/')
ref=$(readelf -h "$TMP/hello" | awk '/Entry point/{print $4}')
check "entry matches readelf" "$mine" "$ref"

exit $fail
