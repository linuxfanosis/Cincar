#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

// A parsed shader program (see tests/shaders/*.ir for the text format).
//
// Values are typed: `float` (inputs, constants, arithmetic results, mutable vars),
// `bool` (results of comparisons) or `vec4`. The parser rejects mixing them up.
//   add sub mul div min max  DST A B     float, float -> float
//   sqrt abs                 DST A       float -> float
//   lt gt le ge eq ne        DST A B     float, float -> bool
//   select                   DST C A B   bool, float, float -> float
//
// Vectors (SSA values only: no vec4 vars, and nothing carried across loops yet):
//   vec4 DST X Y Z W         build a vec4 from four floats
//   get DST V I              component I (a literal 0..3) of a vec4 -> float
//   vadd vsub vmul DST A B   component-wise, vec4 vec4 -> vec4
//   vscale DST V S           vec4 * float -> vec4
//   dot DST A B              vec4 vec4 -> float
//
// Mutable variables and loops:
//   var NAME VALUE           declare a mutable float, initialised from VALUE (outside loops only)
//   set NAME VALUE           assign to a var
//   loop COUNT ... end       repeat the body COUNT times (1..100000); loops cannot nest yet
//   iter NAME                inside a loop: the 0-based iteration index, as a float
// Names defined inside a loop are not visible after it; vars are.
struct Inst {
    // op is one of the operations above, or "var", "set", "loop", "end", "iter".
    // For "loop", `a` holds the count as text; for "get", `b` holds the component index as text.
    // Unused operands are empty.
    std::string op, dst, a, b, c, d;
};
struct Program {
    std::vector<std::string> inputs;                        // input buffers, in binding order
    std::vector<std::pair<std::string, float>> consts;
    std::vector<std::string> vars;                          // mutable variables, in declaration order
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
