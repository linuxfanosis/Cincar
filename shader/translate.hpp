#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

// A parsed shader program (see tests/shaders/*.ir for the text format).
//
// Values are typed: `float` (inputs, constants, arithmetic results) or `bool`
// (results of comparisons). The parser rejects mixing them up.
//   add sub mul div min max  DST A B     float, float -> float
//   sqrt abs                 DST A       float -> float
//   lt gt le ge eq ne        DST A B     float, float -> bool
//   select                   DST C A B   bool, float, float -> float
struct Inst {
    std::string op, dst, a, b, c;   // unused operands are empty
};
struct Program {
    std::vector<std::string> inputs;                        // input buffers, in binding order
    std::vector<std::pair<std::string, float>> consts;
    std::vector<Inst> insts;
    std::string out;                                         // the (float) value written to the output buffer
};

// Parse + validate IR text. On failure returns false with `error` set
// (prefixed by "line N: " when it applies to a specific line).
bool parse_ir(const std::string& ir, Program& prog, std::string& error);

// SPIR-V assembly text (the syntax spirv-as reads) for a Vulkan compute kernel:
// each invocation reads element gl_GlobalInvocationID.x of every input buffer,
// runs the instructions, and writes that element of the output buffer. The element
// count is a push constant: invocations at or past it do nothing (bounds check).
std::string emit_spvasm(const Program& prog);

// Writes a Program back out as IR text (the format parse_ir reads), so tools can be chained.
std::string print_ir(const Program& prog);

// Convenience: parse + emit.
bool translate_ir_to_spvasm(const std::string& ir, std::string& spvasm, std::string& error);

// CPU reference evaluator: runs the program for every element index, using plain
// float arithmetic (bools are 0.0 / 1.0). This is what the translated shader's
// results are checked against.
bool evaluate(const Program& prog, const std::map<std::string, std::vector<float>>& inputs,
              std::vector<float>& out, std::string& error);
