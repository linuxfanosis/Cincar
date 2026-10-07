#pragma once
#include "translate.hpp"
#include <cstdint>
#include <string>
#include <vector>

// A first slice of an AMD RDNA 2 shader-machine-code decoder.
//
// Decodes straight-line programs made of 32-bit VOP2 and VOPC instructions
// (float add/sub/mul/min/max, float compares into VCC, and v_cndmask_b32) into the
// shader IR, so everything downstream (evaluator, SPIR-V, Vulkan) works on real
// RDNA 2 instruction words. Sources are VGPRs, the inline float constants and
// 32-bit literals. Anything else is rejected loudly, with the instruction word.
//
// Calling convention of this harness (NOT the hardware ABI): VGPRs v0..v(num_inputs-1)
// hold the input buffers' elements, and the result is read from VGPR `out_vgpr`.
struct Rdna2Options {
    int num_inputs = 0;
    int out_vgpr = -1;
};

bool decode_rdna2(const std::vector<uint32_t>& words, const Rdna2Options& opts,
                  Program& prog, std::string& error);

// The instructions this decoder understands, for checking against AMD's XML spec.
struct Rdna2Supported {
    const char* name;       // e.g. "V_ADD_F32"
    const char* encoding;   // e.g. "ENC_VOP2" (AMD's name for the encoding)
    int opcode;
};
const std::vector<Rdna2Supported>& rdna2_supported();
