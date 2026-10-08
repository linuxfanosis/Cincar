#include "translate.hpp"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace {

enum class Type { Float, Bool, Var };
// A var is a float as far as users are concerned.
const char* type_name(Type t) { return t == Type::Bool ? "bool" : "float"; }

bool valid_name(const std::string& n) {
    if (n.empty() || (!isalpha((unsigned char)n[0]) && n[0] != '_')) return false;
    for (char c : n) if (!isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

bool is_arith(const std::string& op) {
    return op == "add" || op == "sub" || op == "mul" || op == "div" || op == "min" || op == "max";
}
bool is_unary(const std::string& op) { return op == "sqrt" || op == "abs"; }
bool is_compare(const std::string& op) {
    return op == "lt" || op == "gt" || op == "le" || op == "ge" || op == "eq" || op == "ne";
}

// Float literal that spirv-as will read as a float (always contains '.' or an exponent).
std::string float_literal(float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)v);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

struct Entry { Type type; bool visible; };

} // namespace

bool parse_ir(const std::string& ir, Program& prog, std::string& error) {
    prog = Program{};
    std::map<std::string, Entry> defined;        // every name ever defined (names are never reused)
    int depth = 0;                               // 0 = top level, 1 = inside a loop
    int loop_line = 0;
    std::vector<std::string> loop_names;         // names defined in the current loop

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
        auto define = [&](const std::string& name, Type t) {
            if (!valid_name(name)) return fail(line, "invalid name '" + name + "'");
            if (defined.count(name)) return fail(line, "'" + name + "' is already defined");
            defined[name] = {t, true};
            if (depth == 1) loop_names.push_back(name);
            return true;
        };
        // A value of type `want` (float includes vars) that is visible here.
        auto use = [&](const std::string& name, Type want) {
            auto it = defined.find(name);
            if (it == defined.end()) return fail(line, "undefined value '" + name + "'");
            if (!it->second.visible)
                return fail(line, "'" + name + "' was defined inside a loop and is not visible here");
            bool is_bool = it->second.type == Type::Bool;
            if (is_bool != (want == Type::Bool))
                return fail(line, "'" + name + "' is " + type_name(it->second.type) + ", expected " + type_name(want));
            return true;
        };

        if (op == "in") {
            if (!need(2)) return fail(line, "usage: in NAME");
            if (depth != 0) return fail(line, "'in' must be outside loops");
            if (!define(w[1], Type::Float)) return false;
            prog.inputs.push_back(w[1]);
        } else if (op == "const") {
            if (!need(3)) return fail(line, "usage: const NAME VALUE");
            if (depth != 0) return fail(line, "'const' must be outside loops");
            char* end = nullptr;
            double v = std::strtod(w[2].c_str(), &end);
            if (end == w[2].c_str() || *end != 0) return fail(line, "bad number '" + w[2] + "'");
            if (!define(w[1], Type::Float)) return false;
            prog.consts.push_back({w[1], (float)v});
        } else if (is_arith(op) || is_compare(op)) {
            if (!need(4)) return fail(line, "usage: " + op + " DST A B");
            if (!use(w[2], Type::Float) || !use(w[3], Type::Float)) return false;   // operands first
            if (!define(w[1], is_compare(op) ? Type::Bool : Type::Float)) return false;
            prog.insts.push_back({op, w[1], w[2], w[3], ""});
        } else if (is_unary(op)) {
            if (!need(3)) return fail(line, "usage: " + op + " DST A");
            if (!use(w[2], Type::Float)) return false;
            if (!define(w[1], Type::Float)) return false;
            prog.insts.push_back({op, w[1], w[2], "", ""});
        } else if (op == "select") {
            if (!need(5)) return fail(line, "usage: select DST COND A B");
            if (!use(w[2], Type::Bool) || !use(w[3], Type::Float) || !use(w[4], Type::Float)) return false;
            if (!define(w[1], Type::Float)) return false;
            prog.insts.push_back({op, w[1], w[3], w[4], w[2]});   // c = the condition
        } else if (op == "var") {
            if (!need(3)) return fail(line, "usage: var NAME VALUE");
            if (depth != 0) return fail(line, "'var' must be declared outside loops");
            if (!use(w[2], Type::Float)) return false;
            if (!define(w[1], Type::Var)) return false;
            prog.vars.push_back(w[1]);
            prog.insts.push_back({"var", w[1], w[2], "", ""});
        } else if (op == "set") {
            if (!need(3)) return fail(line, "usage: set NAME VALUE");
            auto it = defined.find(w[1]);
            if (it == defined.end()) return fail(line, "undefined value '" + w[1] + "'");
            if (it->second.type != Type::Var) return fail(line, "'" + w[1] + "' is not a var, so it cannot be assigned");
            if (!use(w[2], Type::Float)) return false;
            prog.insts.push_back({"set", w[1], w[2], "", ""});
        } else if (op == "loop") {
            if (!need(2)) return fail(line, "usage: loop COUNT");
            if (depth != 0) return fail(line, "nested loops are not supported yet");
            char* end = nullptr;
            long n = std::strtol(w[1].c_str(), &end, 10);
            if (end == w[1].c_str() || *end != 0 || n < 1 || n > 100000)
                return fail(line, "bad loop count '" + w[1] + "' (expected an integer from 1 to 100000)");
            depth = 1;
            loop_line = line;
            loop_names.clear();
            prog.insts.push_back({"loop", "", std::to_string(n), "", ""});
        } else if (op == "end") {
            if (!need(1)) return fail(line, "usage: end");
            if (depth != 1) return fail(line, "'end' without a matching 'loop'");
            for (const auto& n : loop_names) defined[n].visible = false;   // loop-local names go out of scope
            depth = 0;
            prog.insts.push_back({"end", "", "", "", ""});
        } else if (op == "iter") {
            if (!need(2)) return fail(line, "usage: iter NAME");
            if (depth != 1) return fail(line, "'iter' is only valid inside a loop");
            if (!define(w[1], Type::Float)) return false;
            prog.insts.push_back({"iter", w[1], "", "", ""});
        } else if (op == "out") {
            if (!need(2)) return fail(line, "usage: out NAME");
            if (depth != 0) return fail(line, "'out' must be outside loops");
            if (!prog.out.empty()) return fail(line, "only one 'out' is supported");
            if (!use(w[1], Type::Float)) return false;
            prog.out = w[1];
        } else {
            return fail(line, "unknown instruction '" + op + "'");
        }
    }
    if (depth != 0) return fail(loop_line, "'loop' is missing its 'end'");
    if (prog.out.empty()) { error = "no 'out' instruction"; return false; }
    return true;
}

std::string emit_spvasm(const Program& p) {
    std::ostringstream o;
    const size_t out_binding = p.inputs.size();
    const std::set<std::string> vars(p.vars.begin(), p.vars.end());
    std::vector<std::string> loop_counts;                    // trip count of each loop, in order
    for (const auto& i : p.insts) if (i.op == "loop") loop_counts.push_back(i.a);

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
      << "%uint_1 = OpConstant %uint 1\n"
      << "%bool = OpTypeBool\n"
      << "%pc_struct = OpTypeStruct %uint\n"
      << "%ptr_pc_struct = OpTypePointer PushConstant %pc_struct\n"
      << "%ptr_pc_uint = OpTypePointer PushConstant %uint\n"
      << "%pc = OpVariable %ptr_pc_struct PushConstant\n"
      << "%ptr_fn_float = OpTypePointer Function %float\n"
      << "%ptr_fn_uint = OpTypePointer Function %uint\n";
    for (const auto& n : p.inputs) o << "%buf_" << n << " = OpVariable %ptr_buf Uniform\n";
    o << "%outbuf = OpVariable %ptr_buf Uniform\n";
    for (const auto& c : p.consts)
        o << "%v_" << c.first << " = OpConstant %float " << float_literal(c.second) << "\n";
    for (size_t k = 0; k < loop_counts.size(); ++k)
        o << "%loop_count_" << k << " = OpConstant %uint " << loop_counts[k] << "\n";

    o << "%main = OpFunction %void None %fn\n"
      << "%entry = OpLabel\n";
    // Function-scope variables must all be declared at the start of the first block.
    for (const auto& n : p.vars) o << "%var_" << n << " = OpVariable %ptr_fn_float Function\n";
    for (size_t k = 0; k < loop_counts.size(); ++k) o << "%loop_i_" << k << " = OpVariable %ptr_fn_uint Function\n";
    o << "%gid_ptr = OpAccessChain %ptr_in_uint %gid %uint_0\n"
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

    int ld = 0;      // counter for the temporaries that read vars
    // The SPIR-V id holding the current value of `name`; for a var this emits a load first.
    auto operand = [&](const std::string& name) -> std::string {
        if (vars.count(name)) {
            std::string id = "%ld_" + std::to_string(ld++);
            o << id << " = OpLoad %float %var_" << name << "\n";
            return id;
        }
        return "%v_" + name;
    };

    int next_loop = 0, cur = -1;
    for (const auto& i : p.insts) {
        if (i.op == "var" || i.op == "set") {
            std::string v = operand(i.a);
            o << "OpStore %var_" << i.dst << " " << v << "\n";
        } else if (i.op == "loop") {
            cur = next_loop++;
            const std::string k = std::to_string(cur);
            o << "OpStore %loop_i_" << k << " %uint_0\n"
              << "OpBranch %loop_header_" << k << "\n"
              << "%loop_header_" << k << " = OpLabel\n"
              << "OpLoopMerge %loop_merge_" << k << " %loop_continue_" << k << " None\n"
              << "OpBranch %loop_cond_" << k << "\n"
              << "%loop_cond_" << k << " = OpLabel\n"
              << "%i_cur_" << k << " = OpLoad %uint %loop_i_" << k << "\n"
              << "%loop_ok_" << k << " = OpULessThan %bool %i_cur_" << k << " %loop_count_" << k << "\n"
              << "OpBranchConditional %loop_ok_" << k << " %loop_body_" << k << " %loop_merge_" << k << "\n"
              << "%loop_body_" << k << " = OpLabel\n";
        } else if (i.op == "end") {
            const std::string k = std::to_string(cur);
            o << "OpBranch %loop_continue_" << k << "\n"
              << "%loop_continue_" << k << " = OpLabel\n"
              << "%i_next_ld_" << k << " = OpLoad %uint %loop_i_" << k << "\n"
              << "%i_next_" << k << " = OpIAdd %uint %i_next_ld_" << k << " %uint_1\n"
              << "OpStore %loop_i_" << k << " %i_next_" << k << "\n"
              << "OpBranch %loop_header_" << k << "\n"
              << "%loop_merge_" << k << " = OpLabel\n";
        } else if (i.op == "iter") {
            std::string t = "%itl_" + std::to_string(ld++);
            o << t << " = OpLoad %uint %loop_i_" << cur << "\n"
              << "%v_" << i.dst << " = OpConvertUToF %float " << t << "\n";
        } else {
            std::string a = operand(i.a);
            std::string b = i.b.empty() ? "" : operand(i.b);
            o << "%v_" << i.dst << " = ";
            auto ab = [&]() { return " " + a + " " + b; };
            if (i.op == "add")       o << "OpFAdd %float" << ab();
            else if (i.op == "sub")  o << "OpFSub %float" << ab();
            else if (i.op == "mul")  o << "OpFMul %float" << ab();
            else if (i.op == "div")  o << "OpFDiv %float" << ab();
            else if (i.op == "min")  o << "OpExtInst %float %glsl FMin" << ab();
            else if (i.op == "max")  o << "OpExtInst %float %glsl FMax" << ab();
            else if (i.op == "sqrt") o << "OpExtInst %float %glsl Sqrt " << a;
            else if (i.op == "abs")  o << "OpExtInst %float %glsl FAbs " << a;
            else if (i.op == "lt")   o << "OpFOrdLessThan %bool" << ab();
            else if (i.op == "gt")   o << "OpFOrdGreaterThan %bool" << ab();
            else if (i.op == "le")   o << "OpFOrdLessThanEqual %bool" << ab();
            else if (i.op == "ge")   o << "OpFOrdGreaterThanEqual %bool" << ab();
            else if (i.op == "eq")   o << "OpFOrdEqual %bool" << ab();
            else if (i.op == "ne")   o << "OpFUnordNotEqual %bool" << ab();   // like C's !=, true for NaN
            else /* select */        o << "OpSelect %float %v_" << i.c << ab();
            o << "\n";
        }
    }
    std::string outv = operand(p.out);
    o << "%outptr = OpAccessChain %ptr_f %outbuf %uint_0 %idx\n"
      << "OpStore %outptr " << outv << "\n"
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

std::string print_ir(const Program& p) {
    std::ostringstream o;
    for (const auto& n : p.inputs) o << "in " << n << "\n";
    for (const auto& k : p.consts) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.9g", (double)k.second);
        o << "const " << k.first << " " << buf << "\n";
    }
    for (const auto& i : p.insts) {
        if (i.op == "select")                       // select DST COND A B
            o << "select " << i.dst << " " << i.c << " " << i.a << " " << i.b << "\n";
        else if (i.op == "var" || i.op == "set")    // var/set NAME VALUE
            o << i.op << " " << i.dst << " " << i.a << "\n";
        else if (i.op == "loop")                    // loop COUNT
            o << "loop " << i.a << "\n";
        else if (i.op == "end")
            o << "end\n";
        else if (i.op == "iter")
            o << "iter " << i.dst << "\n";
        else if (i.b.empty())                       // unary: op DST A
            o << i.op << " " << i.dst << " " << i.a << "\n";
        else                                        // binary / compare: op DST A B
            o << i.op << " " << i.dst << " " << i.a << " " << i.b << "\n";
    }
    o << "out " << p.out << "\n";
    return o.str();
}
