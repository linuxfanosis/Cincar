# Cincar

A small, test-driven study of the building blocks of a console emulator: loading and running
guest code, a system-call/HLE layer, and a GPU shader pipeline that goes from real AMD RDNA 2
machine code to Vulkan. It is an educational project, not a usable emulator.

## What is here

**CPU side** (`loader/`, `runtime/`)
- ELF loader for static, non-PIE x86-64 programs (segments mapped with correct permissions).
- The guest runs in a separate, `ptrace`-traced process; every guest syscall is intercepted and
  handled by our own code (`PTRACE_SYSEMU`), so the guest never reaches the Linux kernel.
- Guest memory comes from one reserved arena managed by our own allocator (`brk`, anonymous `mmap`).
- A proper initial stack (argc/argv/auxv) so a normal static glibc program runs, `printf` included.
- High-level-emulation imports: the guest calls host functions by name; unknown names fail loudly.

**GPU side** (`shader/`)
- A small typed shader IR (floats, bools, `vec4`, mutable vars, counted loops, `select`).
- Translator from the IR to SPIR-V assembly, validated with Khronos `spirv-val`.
- A CPU reference evaluator, and a Vulkan harness (`vkrun`) that runs the translated shader on a
  real Vulkan device and compares the GPU result with the evaluator. It can run under the
  Khronos validation layer.
- An AMD RDNA 2 decoder (`shader/rdna2.cpp`) for a first slice of vector ALU instructions
  (VOP2 and VOPC float arithmetic, compares, `v_cndmask_b32`). It turns real instruction words
  into the IR, so they flow through the same pipeline.

## Method

- **Test first, fail loudly.** Anything unsupported produces an error that names the exact
  syscall, import, line or instruction word. Nothing is silently approximated.
- **Independent oracles.** The RDNA 2 decoder is checked against two things it was not derived from:
  LLVM's assembler (the `.s` files in `tests/rdna2/` must assemble to the checked-in words) and AMD's
  machine-readable ISA specification (opcode numbers).
- **Cross-checking.** Every shader kernel is evaluated on the CPU and, where Vulkan is available,
  on a device, and the results must agree.

## Build and test

Ubuntu 24.04 packages:

    sudo apt-get install -y cmake ninja-build g++ spirv-tools libvulkan-dev mesa-vulkan-drivers

Optional (each enables more checks; missing ones are reported as `SKIP`):

    sudo apt-get install -y vulkan-validationlayers llvm
    python3 tools/explore_isa.py        # downloads AMD's ISA spec into third_party/ (git-ignored)

Then:

    cmake -B build -G Ninja && cmake --build build
    bash tests/run_tests.sh build

The software Vulkan driver (llvmpipe, from `mesa-vulkan-drivers`) is enough to run the GPU tests
without a GPU.

## Tools

| Tool | Purpose |
|---|---|
| `build/elfinfo` | print an ELF's header and program headers |
| `build/runelf FILE` | run a static x86-64 ELF under the runtime (`CINCAR_TRACE=1` logs syscalls and imports) |
| `build/shadercc FILE.ir` | translate shader IR to SPIR-V assembly |
| `build/shaderrun FILE.ir name=v1,v2 ...` | run the CPU reference evaluator |
| `build/vkrun FILE.ir name=v1,v2 ...` | run on a Vulkan device and compare with the CPU (`VKRUN_VALIDATE=1` for the validation layer) |
| `build/rdna2dec FILE.hex --inputs N --out vK` | decode RDNA 2 instruction words into shader IR |
| `tools/asm_rdna2.py`, `tools/check_isa_opcodes.py`, `tools/explore_isa.py` | oracle and spec helpers |

## Limits

- Guests: static non-PIE x86-64 ELF only; a handful of syscalls; no dynamic linking, no threads, no files.
- Shaders: straight-line and single-level counted loops; scalar float buffers; no textures, no
  descriptors beyond plain storage buffers, no real graphics pipeline.
- RDNA 2 decoder: 32-bit VOP2 and VOPC float instructions only, with a harness calling convention
  (inputs in `v0..`, result in a chosen VGPR) that is not the hardware ABI. No memory instructions,
  scalar registers, control flow or 64-bit encodings yet.
- Tested on llvmpipe (software Vulkan). Not yet run on real GPU hardware.

## Legal and clean-room notes

- No console firmware, keys, executables or game data are, or should ever be, in this repository.
  Tests use programs and shaders written for this project.
- AMD's machine-readable ISA specification is published under the MIT license. It is downloaded on
  demand into `third_party/` (git-ignored) and not redistributed here.
- Add your own LICENSE file before accepting contributions or sharing the code.
