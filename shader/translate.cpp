#include "translate.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <vector>

namespace {

struct Inst {
    std::string op, dst, a, b;   // add/sub/mul: dst = a op b
};

bool valid_name(const std::string& n) {
    if (n.empty() || (!isalpha((unsigned char)n[0]) && n[0] != '_')) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// Float literal that spirv-as will read as a float (always contains '.' or an exponent).
std::string float_literal(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

} // namespace

bool translate_ir_to_spvasm(const std::string& ir, std::string& out, std::string& error) {
    std::vector<std::string> inputs;           // input buffer names, in binding order
    std::map<std::string, double> consts;      // const name -> value
    std::vector<std::string> const_order;
    std::vector<Inst> insts;
    std::map<std::string, int> defined;        // every value name -> line where defined
    std::string out_name;

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
            inputs.push_back(w[1]);
        } else if (op == "const") {
            if (!need(3)) return fail(line, "usage: const NAME VALUE");
            char* end = nullptr;
            double v = std::strtod(w[2].c_str(), &end);
            if (end == w[2].c_str() || *end != 0) return fail(line, "bad number '" + w[2] + "'");
            if (!define(w[1])) return false;
            consts[w[1]] = v;
            const_order.push_back(w[1]);
        } else if (op == "add" || op == "sub" || op == "mul") {
            if (!need(4)) return fail(line, "usage: " + op + " DST A B");
            if (!use(w[2]) || !use(w[3])) return false;   // operands first: no self-reference
            if (!define(w[1])) return false;
            insts.push_back({op, w[1], w[2], w[3]});
        } else if (op == "out") {
            if (!need(2)) return fail(line, "usage: out NAME");
            if (!out_name.empty()) return fail(line, "only one 'out' is supported");
            if (!use(w[1])) return false;
            out_name = w[1];
        } else {
            return fail(line, "unknown instruction '" + op + "'");
        }
    }
    if (out_name.empty()) { error = "no 'out' instruction"; return false; }

    // ---- emit SPIR-V assembly -------------------------------------------------
    std::ostringstream o;
    const size_t out_binding = inputs.size();

    o << "OpCapability Shader\n"
      << "OpMemoryModel Logical GLSL450\n"
      << "OpEntryPoint GLCompute %main \"main\" %gid\n"
      << "OpExecutionMode %main LocalSize 64 1 1\n"
      << "OpDecorate %gid BuiltIn GlobalInvocationId\n"
      << "OpDecorate %arr ArrayStride 4\n"
      << "OpMemberDecorate %buf 0 Offset 0\n"
      << "OpDecorate %buf BufferBlock\n";
    for (size_t i = 0; i < inputs.size(); ++i)
        o << "OpDecorate %buf_" << inputs[i] << " DescriptorSet 0\n"
          << "OpDecorate %buf_" << inputs[i] << " Binding " << i << "\n";
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
      << "%uint_0 = OpConstant %uint 0\n";
    for (const auto& n : inputs) o << "%buf_" << n << " = OpVariable %ptr_buf Uniform\n";
    o << "%outbuf = OpVariable %ptr_buf Uniform\n";
    for (const auto& n : const_order)
        o << "%v_" << n << " = OpConstant %float " << float_literal(consts[n]) << "\n";

    o << "%main = OpFunction %void None %fn\n"
      << "%entry = OpLabel\n"
      << "%gid_ptr = OpAccessChain %ptr_in_uint %gid %uint_0\n"
      << "%idx = OpLoad %uint %gid_ptr\n";
    for (const auto& n : inputs)
        o << "%p_" << n << " = OpAccessChain %ptr_f %buf_" << n << " %uint_0 %idx\n"
          << "%v_" << n << " = OpLoad %float %p_" << n << "\n";
    for (const auto& i : insts) {
        const char* spv = i.op == "add" ? "OpFAdd" : i.op == "sub" ? "OpFSub" : "OpFMul";
        o << "%v_" << i.dst << " = " << spv << " %float %v_" << i.a << " %v_" << i.b << "\n";
    }
    o << "%outptr = OpAccessChain %ptr_f %outbuf %uint_0 %idx\n"
      << "OpStore %outptr %v_" << out_name << "\n"
      << "OpReturn\n"
      << "OpFunctionEnd\n";

    out = o.str();
    return true;
}
