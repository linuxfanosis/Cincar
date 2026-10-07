#!/usr/bin/env bash
set -u
BUILD=${1:-build}
TMP=$(mktemp -d)
fail=0

check() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1 (expected '$3', got '$2')"; fail=1; fi
}

for t in hello exit7 getpid crash escape memory badmmap; do
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
check "exit intercepted" "$(echo "$trace" | grep -c 'guest\] exit(0,')" "1"

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

# Guest memory: brk + anonymous mmap come from our own allocator.
out=$("$BUILD/runelf" "$TMP/memory"); code=$?
check "brk/mmap guest output" "$out" "OK"
check "brk/mmap guest exit code" "$code" "0"
trace=$(CINCAR_TRACE=1 "$BUILD/runelf" "$TMP/memory" 2>&1 >/dev/null)
check "brk intercepted" "$(echo "$trace" | grep -c 'guest\] brk(')" "2"
check "mmap intercepted" "$(echo "$trace" | grep -c 'guest\] mmap(')" "1"
"$BUILD/runelf" "$TMP/badmmap" 2>/dev/null; code=$?
check "file-backed mmap rejected (EINVAL)" "$code" "22"

# A real C program: libc startup (TLS, auxv, brk, ...) then printf, all via our runtime.
gcc -static -no-pie -O1 tests/cprog.c -o "$TMP/cprog"
out=$("$BUILD/runelf" "$TMP/cprog" 2>&1); code=$?
check "C program output" "$out" "hello from C, via libc"
check "C program exit code" "$code" "0"
trace=$(CINCAR_TRACE=1 "$BUILD/runelf" "$TMP/cprog" 2>&1 >/dev/null)
check "libc set up TLS via arch_prctl" "$(echo "$trace" | grep -c 'guest\] arch_prctl(4098')" "1"

# HLE imports: the guest calls host functions by name; unknown names fail loudly.
gcc -static -no-pie tests/hle_guest.c tests/hle_stubs.S -o "$TMP/hle"
out=$("$BUILD/runelf" "$TMP/hle" 2>/dev/null); code=$?
check "HLE puts output" "$out" "hello from an HLE import"
check "HLE guest exit code" "$code" "0"
msg=$(CINCAR_TRACE=1 "$BUILD/runelf" "$TMP/hle" 2>&1 >/dev/null)
check "HLE add resolved by name" "$(echo "$msg" | grep -c 'hle\] cincar_add(2, 3)')" "1"
check "unresolved import named" "$(echo "$msg" | grep -c "unresolved import 'cincar_missing'")" "1"

# GPU track: IR -> SPIR-V assembly, then validate with the real Khronos tools.
for name in madd names; do
    "$BUILD/shadercc" tests/shaders/$name.ir > "$TMP/$name.spvasm" 2>/dev/null; code=$?
    check "shadercc translates $name.ir" "$code" "0"
done
if command -v spirv-as >/dev/null && command -v spirv-val >/dev/null; then
    for name in madd names; do
        spirv-as --target-env vulkan1.0 "$TMP/$name.spvasm" -o "$TMP/$name.spv" 2>"$TMP/as.log"; code=$?
        check "spirv-as assembles $name" "$code" "0"
        [ $code -ne 0 ] && cat "$TMP/as.log"
        spirv-val --target-env vulkan1.0 "$TMP/$name.spv" 2>"$TMP/val.log"; code=$?
        check "spirv-val accepts $name" "$code" "0"
        [ $code -ne 0 ] && cat "$TMP/val.log"
    done
else
    echo "SKIP: spirv-as/spirv-val not installed (sudo apt-get install -y spirv-tools)"
fi
expect_ir_error() {   # file, expected message
    err=$("$BUILD/shadercc" "tests/shaders/$1.ir" 2>&1 >/dev/null); code=$?
    check "IR error: $1 exit code" "$code" "1"
    check "IR error: $1 message" "$err" "$2"
}
expect_ir_error bad_undefined "error: line 2: undefined value 'missing'"
expect_ir_error bad_noout "error: no 'out' instruction"
expect_ir_error bad_duplicate "error: line 2: 'a' is already defined"
expect_ir_error bad_opcode "error: line 2: unknown instruction 'frobnicate'"

echo "not an elf" > "$TMP/bad"
err=$("$BUILD/runelf" "$TMP/bad" 2>&1); code=$?
check "bad file rejected" "$code" "1"
check "bad file message" "$err" "error: file too small for an ELF64 header"

mine=$("$BUILD/elfinfo" "$TMP/hello" | head -1 | sed -E 's/.*entry (0x[0-9a-f]+).*/\1/')
ref=$(readelf -h "$TMP/hello" | awk '/Entry point/{print $4}')
check "entry matches readelf" "$mine" "$ref"

exit $fail
