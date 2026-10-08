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
for name in madd names ops select cmp loop_sum newton_sqrt two_loops vec_dot vec_ops; do
    "$BUILD/shadercc" tests/shaders/$name.ir > "$TMP/$name.spvasm" 2>/dev/null; code=$?
    check "shadercc translates $name.ir" "$code" "0"
done
if command -v spirv-as >/dev/null && command -v spirv-val >/dev/null; then
    for name in madd names ops select cmp loop_sum newton_sqrt two_loops vec_dot vec_ops; do
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
# The generated shader must be bounds-checked (the element count arrives as a push constant).
check "emitted shader has a bounds check" "$(grep -c 'OpULessThan' "$TMP/madd.spvasm")" "1"
check "emitted shader reads the count from a push constant" "$(grep -c 'OpVariable %ptr_pc_struct PushConstant' "$TMP/madd.spvasm")" "1"

# CPU reference evaluator: the numbers a correct shader must produce.
check "evaluator: madd" "$("$BUILD/shaderrun" tests/shaders/madd.ir a=1,2,3 b=4,5,6)" "10 14 18"
check "evaluator: every operation" "$("$BUILD/shaderrun" tests/shaders/ops.ir x=16,9 y=8,100)" "2 -21"
check "evaluator: names that look like internal ids" "$("$BUILD/shaderrun" tests/shaders/names.ir out=1,2 count=3,4 body=5,6)" "10 13"
check "evaluator: select (clamp)" "$("$BUILD/shaderrun" tests/shaders/select.ir x=-5,3,10 limit=8,8,8)" "0 3 8"
check "evaluator: all six comparisons" "$("$BUILD/shaderrun" tests/shaders/cmp.ir a=1,2,3 b=2,2,1)" "101001 1110 10101"
check "emitted loop has a structured loop header" "$(grep -c 'OpLoopMerge' "$TMP/loop_sum.spvasm")" "1"
check "two loops get two loop headers" "$(grep -c 'OpLoopMerge' "$TMP/two_loops.spvasm")" "2"
check "evaluator: loop with accumulator and iteration index" "$("$BUILD/shaderrun" tests/shaders/loop_sum.ir x=1,2,3)" "10 20 30"
check "evaluator: Newton square root" "$("$BUILD/shaderrun" tests/shaders/newton_sqrt.ir x=4,9,2)" "2 3 1.41421"
check "evaluator: two loops in a row" "$("$BUILD/shaderrun" tests/shaders/two_loops.ir x=1,2,10)" "7 11 43"
check "emitted vec4 kernel builds vectors" "$(grep -c 'OpCompositeConstruct' "$TMP/vec_dot.spvasm")" "2"
check "emitted vec4 kernel uses OpDot" "$(grep -c 'OpDot' "$TMP/vec_dot.spvasm")" "1"
check "evaluator: vec4 dot product" "$("$BUILD/shaderrun" tests/shaders/vec_dot.ir x=1,2,0 y=1,0,0 z=1,0,1)" "14 9 9"
check "evaluator: vec4 add/sub/mul/scale/get" "$("$BUILD/shaderrun" tests/shaders/vec_ops.ir a=3,1,5 b=5,7,2)" "12472 24216 2605.5"
expect_eval_error() {   # expected message, then shaderrun arguments
    msg="$1"; shift
    err=$("$BUILD/shaderrun" "$@" 2>&1 >/dev/null); code=$?
    check "evaluator error: $msg (exit code)" "$code" "1"
    check "evaluator error: $msg" "$err" "error: $msg"
}
expect_eval_error "input 'b' has a different length" tests/shaders/madd.ir a=1,2 b=1
expect_eval_error "missing data for input 'b'" tests/shaders/madd.ir a=1,2
expect_eval_error "data given for unknown input 'c'" tests/shaders/madd.ir a=1 b=1 c=1
expect_eval_error "bad number 'x'" tests/shaders/madd.ir a=1,x b=1,2

# Run the translated shader on a real Vulkan device (lavapipe on CPU, or a GPU) and
# compare with the CPU evaluator. Skipped when there is no Vulkan device.
gpu_check() {   # test name, then vkrun arguments
    gname="$1"; shift
    gout=$("$BUILD/vkrun" "$@" 2>&1); gcode=$?
    if [ $gcode -eq 77 ]; then echo "SKIP: GPU $gname (no Vulkan device)"; return; fi
    check "GPU matches CPU: $gname" "$gcode" "0"
    if [ $gcode -ne 0 ]; then echo "$gout"; fi
}
if [ -x "$BUILD/vkrun" ] && command -v spirv-as >/dev/null; then
    A=$(seq -s, 1 100); B=$(seq -s, 101 200)
    gpu_check "madd" tests/shaders/madd.ir a=1,2,3 b=4,5,6
    gpu_check "all operations" tests/shaders/ops.ir x=16,9 y=8,100
    gpu_check "madd, 100 elements (2 workgroups)" tests/shaders/madd.ir a=$A b=$B
    gpu_check "madd, 1 element (63 idle threads)" tests/shaders/madd.ir a=7 b=8
    gpu_check "madd, 64 elements (exactly one workgroup)" tests/shaders/madd.ir a=$(seq -s, 1 64) b=$(seq -s, 1 64)
    gpu_check "input names that look like internal ids" tests/shaders/names.ir out=1,2 count=3,4 body=5,6
    gpu_check "select (clamp)" tests/shaders/select.ir x=-5,3,10 limit=8,8,8
    gpu_check "all six comparisons" tests/shaders/cmp.ir a=1,2,3 b=2,2,1
    X=$(seq -s, -50 49); L=$(printf '20,%.0s' $(seq 100)); L=${L%,}
    gpu_check "select (clamp), 100 elements" tests/shaders/select.ir x=$X limit=$L
    gpu_check "loop with accumulator" tests/shaders/loop_sum.ir x=1,2,3
    gpu_check "two loops in a row" tests/shaders/two_loops.ir x=1,2,10
    gpu_check "Newton square root, 100 elements" tests/shaders/newton_sqrt.ir x=$(seq -s, 1 100)
    gpu_check "vec4 dot product" tests/shaders/vec_dot.ir x=1,2,0 y=1,0,0 z=1,0,1
    gpu_check "vec4 add/sub/mul/scale/get" tests/shaders/vec_ops.ir a=3,1,5 b=5,7,2
    gpu_check "vec4 add/sub/mul/scale/get, 100 elements" tests/shaders/vec_ops.ir a=$(seq -s, 1 100) b=$(seq -s, 101 200)
    # The same harness under the Khronos validation layer: any warning or error fails the run.
    if [ -f /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json ]; then
        export VKRUN_VALIDATE=1
        gpu_check "under the validation layer: madd" tests/shaders/madd.ir a=1,2,3 b=4,5,6
        gpu_check "under the validation layer: select" tests/shaders/select.ir x=-5,3,10 limit=8,8,8
        gpu_check "under the validation layer: loop" tests/shaders/loop_sum.ir x=1,2,3
        gpu_check "under the validation layer: vec4, 100 elements" tests/shaders/vec_ops.ir a=$(seq -s, 1 100) b=$(seq -s, 101 200)
        unset VKRUN_VALIDATE
    else
        echo "SKIP: Vulkan validation layer not installed (sudo apt-get install -y vulkan-validationlayers)"
    fi
else
    echo "SKIP: vkrun not built or spirv-as missing"
fi

# ---- RDNA 2 machine-code decoder ------------------------------------------------------
# Real instruction words -> shader IR -> evaluator (and Vulkan, when available).
rdna2_run() {   # program, number of inputs, output register, expected CPU result, shaderrun args...
    rname="$1"; rin="$2"; rout="$3"; rexp="$4"; shift 4
    "$BUILD/rdna2dec" "tests/rdna2/$rname.hex" --inputs "$rin" --out "$rout" > "$TMP/$rname.ir" 2> "$TMP/$rname.err"; rcode=$?
    check "RDNA2 decode $rname" "$rcode" "0"
    if [ $rcode -ne 0 ]; then cat "$TMP/$rname.err"; fi
    check "RDNA2 $rname: CPU result" "$("$BUILD/shaderrun" "$TMP/$rname.ir" "$@")" "$rexp"
    if [ -x "$BUILD/vkrun" ] && command -v spirv-as >/dev/null; then
        gpu_check "RDNA2 $rname" "$TMP/$rname.ir" "$@"
    fi
}
rdna2_run madd  2 v3  "10 14 18"          v0=1,2,3 v1=4,5,6
rdna2_run minxy 2 v2  "1 2 5"             v0=1,9,5 v1=4,2,5
rdna2_run lit   2 v3  "150000 225000"     v0=1,2 v1=0.5,0.25
rdna2_run cmp6  2 v19 "101001 1110 10101" v0=1,2,3 v1=2,2,1

rdna2_error() {   # program, number of inputs, output register, expected message
    err=$("$BUILD/rdna2dec" "tests/rdna2/$1.hex" --inputs "$2" --out "$3" 2>&1 >/dev/null); code=$?
    check "RDNA2 error: $1 (exit code)" "$code" "1"
    check "RDNA2 error: $1" "$err" "error: $4"
}
rdna2_error bad_sdwa     2 v2 "word 0 (0x060402fa): SDWA/DPP encodings are not supported"
rdna2_error bad_uninit   2 v2 "word 0 (0x06040b00): reads v5 before it is written"
rdna2_error bad_opcode   2 v2 "word 0 (0x0a040300): unsupported VOP2 opcode 5"
rdna2_error bad_encoding 2 v2 "word 0 (0xbf800000): unsupported instruction encoding (only VOP2 and VOPC are decoded so far)"
rdna2_error bad_literal  2 v2 "word 0 (0x060402ff): literal constant is missing (end of program)"
rdna2_error bad_vcc      2 v2 "word 0 (0x02040101): v_cndmask_b32 reads VCC before any compare wrote it"
rdna2_error madd         2 v9 "output register v9 was never written (and is not an input)"

# Oracle 1: LLVM's assembler must produce exactly the checked-in words for each .s file.
if python3 tools/asm_rdna2.py --check >/dev/null 2>&1; then
    for s in tests/rdna2/*.s; do
        b=$(basename "$s" .s)
        mine=$(python3 tools/asm_rdna2.py "$s" 2>&1)
        want=$(sed 's/#.*//' "tests/rdna2/$b.hex" | tr -s ' \t' '\n' | grep -v '^$' | tr 'A-F' 'a-f')
        check "LLVM assembles $b to the checked-in words" "$mine" "$want"
    done
else
    echo "SKIP: llvm-mc with AMDGPU support not found (sudo apt-get install -y llvm)"
fi

# Oracle 2: opcode numbers must match AMD's machine-readable ISA spec (download: python3 tools/explore_isa.py).
python3 tools/check_isa_opcodes.py "$BUILD/rdna2dec" > "$TMP/isa.log" 2>&1; code=$?
if [ $code -eq 77 ]; then
    echo "SKIP: AMD ISA spec not downloaded (python3 tools/explore_isa.py)"
else
    check "decoder opcodes match AMD's XML spec" "$code" "0"
    if [ $code -ne 0 ]; then cat "$TMP/isa.log"; fi
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
expect_ir_error bad_float_as_bool "error: line 2: 'a' is float, expected bool"
expect_ir_error bad_bool_in_arith "error: line 3: 'c' is bool, expected float"
expect_ir_error bad_bool_output "error: line 3: 'c' is bool, expected float"
expect_ir_error bad_loop_scope "error: line 5: 't' was defined inside a loop and is not visible here"
expect_ir_error bad_nested "error: line 3: nested loops are not supported yet"
expect_ir_error bad_end "error: line 2: 'end' without a matching 'loop'"
expect_ir_error bad_noend "error: line 2: 'loop' is missing its 'end'"
expect_ir_error bad_set_nonvar "error: line 2: 'x' is not a var, so it cannot be assigned"
expect_ir_error bad_iter_outside "error: line 2: 'iter' is only valid inside a loop"
expect_ir_error bad_var_in_loop "error: line 3: 'var' must be declared outside loops"
expect_ir_error bad_loop_count "error: line 2: bad loop count '0' (expected an integer from 1 to 100000)"
expect_ir_error bad_vec_as_float "error: line 3: 'v' is vec4, expected float"
expect_ir_error bad_float_as_vec "error: line 3: 'a' is float, expected vec4"
expect_ir_error bad_vec_out "error: line 3: 'v' is vec4, expected float"
expect_ir_error bad_get_index "error: line 3: bad component index '4' (expected 0, 1, 2 or 3)"
expect_ir_error bad_vec4_args "error: line 2: usage: vec4 DST X Y Z W"

echo "not an elf" > "$TMP/bad"
err=$("$BUILD/runelf" "$TMP/bad" 2>&1); code=$?
check "bad file rejected" "$code" "1"
check "bad file message" "$err" "error: file too small for an ELF64 header"

mine=$("$BUILD/elfinfo" "$TMP/hello" | head -1 | sed -E 's/.*entry (0x[0-9a-f]+).*/\1/')
ref=$(readelf -h "$TMP/hello" | awk '/Entry point/{print $4}')
check "entry matches readelf" "$mine" "$ref"

exit $fail
