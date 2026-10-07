#include "translate.hpp"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace {

bool valid_name(const std::string& n) {
    if (n.empty() || (!isalpha((unsigned char)n[0]) && n[0] != '_')) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

bool is_binary(const std::string& op) {
    return op == "add" || op == "sub" || op == "mul" || op == "div" || op == "min" || op == "max";
}
bool is_unary(const std::string& op) { return op == "sqrt" || op == "abs"; }

// Float literal that spirv-as will read as a float (always contains '.' or an exponent).
std::string float_literal(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)v);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

} // namespace

bool parse_ir(const std::string& ir, Program& prog, std::string& error) {
    prog = Program{};
    std::map<std::string, int> defined;

    auto fail = [&](int line, const std::string& msg) {
        error = "line " + std::to_string(line) + ": " + msg;
        return false;
    };

    std::istringstream in(ir);
    std::string text;
    int line = 0;
    while (std::getline(in, text)) {
        ++line;
        auto hash = text.find('#');
        if (hash != std::string::npos) text.erase(hash);
        std::istringstream ls(text);
        std::vector<std::string> w;
        for (std::string t; ls >> t;) w.push_back(t);
        if (w.empty()) continue;

        const std::string& op = w[0];
        auto need = [&](size_t n) { return w.size() == n; };
        auto define = [&](const std::string& name) {
            if (!valid_name(name)) return fail(line, "invalid name '" + name + "'");
            if (defined.count(name)) return fail(line, "'" + name + "' is already defined");
            defined[name] = line;
            return true;
        };
        auto use = [&](const std::string& name) {
            if (!defined.count(name)) return fail(line, "undefined value '" + name + "'");
            return true;
        };

        if (op == "in") {
            if (!need(2)) return fail(line, "usage: in NAME");
            if (!define(w[1])) return false;
            prog.inputs.push_back(w[1]);
        } else if (op == "const") {
            if (!need(3)) return fail(line, "usage: const NAME VALUE");
            char* end = nullptr;
            double v = std::strtod(w[2].c_str(), &end);
            if (end == w[2].c_str() || *end != 0) return fail(line, "bad number '" + w[2] + "'");
            if (!define(w[1])) return false;
            prog.consts.push_back({w[1], (float)v});
        } else if (is_binary(op)) {
            if (!need(4)) return fail(line, "usage: " + op + " DST A B");
            if (!use(w[2]) || !use(w[3])) return false;   // operands first: no self-reference
            if (!define(w[1])) return false;
            prog.insts.push_back({op, w[1], w[2], w[3]});
        } else if (is_unary(op)) {
            if (!need(3)) return fail(line, "usage: " + op + " DST A");
            if (!use(w[2])) return false;
            if (!define(w[1])) return false;
            prog.insts.push_back({op, w[1], w[2], ""});
        } else if (op == "out") {
            if (!need(2)) return fail(line, "usage: out NAME");
            if (!prog.out.empty()) return fail(line, "only one 'out' is supported");
            if (!use(w[1])) return false;
            prog.out = w[1];
        } else {
            return fail(line, "unknown instruction '" + op + "'");
        }
    }
    if (prog.out.empty()) { error = "no 'out' instruction"; return false; }
    return true;
}

std::string emit_spvasm(const Program& p) {
    std::ostringstream o;
    const size_t out_binding = p.inputs.size();

    o << "OpCapability Shader\n"
      << "%glsl = OpExtInstImport \"GLSL.std.450\"\n"
      << "OpMemoryModel Logical GLSL450\n"
      << "OpEntryPoint GLCompute %main \"main\" %gid\n"
      << "OpExecutionMode %main LocalSize 64 1 1\n"
      << "OpDecorate %gid BuiltIn GlobalInvocationId\n"
      << "OpDecorate %arr ArrayStride 4\n"
      << "OpMemberDecorate %buf 0 Offset 0\n"
      << "OpDecorate %buf BufferBlock\n"
      << "OpDecorate %pc_struct Block\n"
      << "OpMemberDecorate %pc_struct 0 Offset 0\n";
    for (size_t i = 0; i < p.inputs.size(); ++i)
        o << "OpDecorate %buf_" << p.inputs[i] << " DescriptorSet 0\n"
          << "OpDecorate %buf_" << p.inputs[i] << " Binding " << i << "\n";
    o << "OpDecorate %outbuf DescriptorSet 0\n"
      << "OpDecorate %outbuf Binding " << out_binding << "\n";

    o << "%void = OpTypeVoid\n"
      << "%fn = OpTypeFunction %void\n"
      << "%float = OpTypeFloat 32\n"
      << "%uint = OpTypeInt 32 0\n"
      << "%v3uint = OpTypeVector %uint 3\n"
      << "%ptr_in_v3uint = OpTypePointer Input %v3uint\n"
      << "%ptr_in_uint = OpTypePointer Input %uint\n"
      << "%gid = OpVariable %ptr_in_v3uint Input\n"
      << "%arr = OpTypeRuntimeArray %float\n"
      << "%buf = OpTypeStruct %arr\n"
      << "%ptr_buf = OpTypePointer Uniform %buf\n"
      << "%ptr_f = OpTypePointer Uniform %float\n"
      << "%uint_0 = OpConstant %uint 0\n"
      << "%bool = OpTypeBool\n"
      << "%pc_struct = OpTypeStruct %uint\n"
      << "%ptr_pc_struct = OpTypePointer PushConstant %pc_struct\n"
      << "%ptr_pc_uint = OpTypePointer PushConstant %uint\n"
      << "%pc = OpVariable %ptr_pc_struct PushConstant\n";
    for (const auto& n : p.inputs) o << "%buf_" << n << " = OpVariable %ptr_buf Uniform\n";
    o << "%outbuf = OpVariable %ptr_buf Uniform\n";
    for (const auto& c : p.consts)
        o << "%v_" << c.first << " = OpConstant %float " << float_literal(c.second) << "\n";

    o << "%main = OpFunction %void None %fn\n"
      << "%entry = OpLabel\n"
      << "%gid_ptr = OpAccessChain %ptr_in_uint %gid %uint_0\n"
      << "%idx = OpLoad %uint %gid_ptr\n"
      // Bounds check: only threads with idx < count (a push constant) do any work.
      << "%count_ptr = OpAccessChain %ptr_pc_uint %pc %uint_0\n"
      << "%count = OpLoad %uint %count_ptr\n"
      << "%in_range = OpULessThan %bool %idx %count\n"
      << "OpSelectionMerge %merge None\n"
      << "OpBranchConditional %in_range %body %merge\n"
      << "%body = OpLabel\n";
    for (const auto& n : p.inputs)
        o << "%p_" << n << " = OpAccessChain %ptr_f %buf_" << n << " %uint_0 %idx\n"
          << "%v_" << n << " = OpLoad %float %p_" << n << "\n";
    for (const auto& i : p.insts) {
        o << "%v_" << i.dst << " = ";
        if (i.op == "add")       o << "OpFAdd %float %v_" << i.a << " %v_" << i.b;
        else if (i.op == "sub")  o << "OpFSub %float %v_" << i.a << " %v_" << i.b;
        else if (i.op == "mul")  o << "OpFMul %float %v_" << i.a << " %v_" << i.b;
        else if (i.op == "div")  o << "OpFDiv %float %v_" << i.a << " %v_" << i.b;
        else if (i.op == "min")  o << "OpExtInst %float %glsl FMin %v_" << i.a << " %v_" << i.b;
        else if (i.op == "max")  o << "OpExtInst %float %glsl FMax %v_" << i.a << " %v_" << i.b;
        else if (i.op == "sqrt") o << "OpExtInst %float %glsl Sqrt %v_" << i.a;
        else                     o << "OpExtInst %float %glsl FAbs %v_" << i.a;   // abs
        o << "\n";
    }
    o << "%outptr = OpAccessChain %ptr_f %outbuf %uint_0 %idx\n"
      << "OpStore %outptr %v_" << p.out << "\n"
      << "OpBranch %merge\n"
      << "%merge = OpLabel\n"
      << "OpReturn\n"
      << "OpFunctionEnd\n";
    return o.str();
}

bool translate_ir_to_spvasm(const std::string& ir, std::string& spvasm, std::string& error) {
    Program p;
    if (!parse_ir(ir, p, error)) return false;
    spvasm = emit_spvasm(p);
    return true;
}
