#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

// A parsed shader program (see tests/shaders/*.ir for the text format).
struct Inst {
    std::string op, dst, a, b;   // b is empty for unary ops (sqrt, abs)
};
struct Program {
    std::vector<std::string> inputs;                        // input buffers, in binding order
    std::vector<std::pair<std::string, float>> consts;
    std::vector<Inst> insts;
    std::string out;                                         // the value written to the output buffer
};

// Parse + validate IR text. On failure returns false with `error` set
// (prefixed by "line N: " when it applies to a specific line).
bool parse_ir(const std::string& ir, Program& prog, std::string& error);

// SPIR-V assembly text (the syntax spirv-as reads) for a Vulkan compute kernel:
// each invocation reads element gl_GlobalInvocationID.x of every input buffer,
// runs the instructions, and writes that element of the output buffer.
std::string emit_spvasm(const Program& prog);

// Convenience: parse + emit.
bool translate_ir_to_spvasm(const std::string& ir, std::string& spvasm, std::string& error);

// CPU reference evaluator: runs the program for every element index, using plain
// float arithmetic. This is what the translated shader's results are checked against.
bool evaluate(const Program& prog, const std::map<std::string, std::vector<float>>& inputs,
              std::vector<float>& out, std::string& error);
