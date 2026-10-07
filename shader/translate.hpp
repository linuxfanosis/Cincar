#pragma once
#include <string>

// Translates a tiny shader IR (see tests/shaders/*.ir) into SPIR-V assembly
// text (the syntax spirv-as reads). Returns false with `error` set (prefixed by
// a line number when it applies to a specific line) if the IR is invalid.
//
// The generated shader is a Vulkan compute kernel: every invocation reads element
// gl_GlobalInvocationID.x from each input buffer, runs the instructions, and
// writes element gl_GlobalInvocationID.x of the output buffer.
bool translate_ir_to_spvasm(const std::string& ir, std::string& spvasm, std::string& error);
