#include "translate.hpp"
#include <cmath>
#include <cstdlib>

bool evaluate(const Program& p, const std::map<std::string, std::vector<float>>& inputs,
              std::vector<float>& out, std::string& error) {
    if (p.inputs.empty()) { error = "program has no inputs, so there is nothing to iterate over"; return false; }
    size_t n = 0;
    bool first = true;
    for (const auto& name : p.inputs) {
        auto it = inputs.find(name);
        if (it == inputs.end()) { error = "missing data for input '" + name + "'"; return false; }
        if (first) { n = it->second.size(); first = false; }
        else if (it->second.size() != n) { error = "input '" + name + "' has a different length"; return false; }
    }
    for (const auto& kv : inputs) {
        bool known = false;
        for (const auto& name : p.inputs) known = known || name == kv.first;
        if (!known) { error = "data given for unknown input '" + kv.first + "'"; return false; }
    }

    out.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        // Bools are stored as 0.0 / 1.0 and vars are plain entries that `set` overwrites
        // (the parser guarantees every name is used correctly).
        std::map<std::string, float> v;
        for (const auto& name : p.inputs) v[name] = inputs.at(name)[i];
        for (const auto& c : p.consts) v[c.first] = c.second;

        size_t pc = 0, loop_start = 0;
        long iter = 0, count = 0;
        while (pc < p.insts.size()) {
            const Inst& s = p.insts[pc];
            if (s.op == "loop") { count = std::atol(s.a.c_str()); iter = 0; loop_start = pc; ++pc; continue; }
            if (s.op == "end")  { ++iter; pc = iter < count ? loop_start + 1 : pc + 1; continue; }
            if (s.op == "iter") { v[s.dst] = (float)iter; ++pc; continue; }
            if (s.op == "var" || s.op == "set") { v[s.dst] = v.at(s.a); ++pc; continue; }

            float a = s.a.empty() ? 0.0f : v.at(s.a);
            float b = s.b.empty() ? 0.0f : v.at(s.b);
            float r;
            if (s.op == "add") r = a + b;
            else if (s.op == "sub") r = a - b;
            else if (s.op == "mul") r = a * b;
            else if (s.op == "div") r = a / b;
            else if (s.op == "min") r = std::fmin(a, b);
            else if (s.op == "max") r = std::fmax(a, b);
            else if (s.op == "sqrt") r = std::sqrt(a);
            else if (s.op == "abs") r = std::fabs(a);
            else if (s.op == "lt") r = a < b ? 1.0f : 0.0f;
            else if (s.op == "gt") r = a > b ? 1.0f : 0.0f;
            else if (s.op == "le") r = a <= b ? 1.0f : 0.0f;
            else if (s.op == "ge") r = a >= b ? 1.0f : 0.0f;
            else if (s.op == "eq") r = a == b ? 1.0f : 0.0f;
            else if (s.op == "ne") r = a != b ? 1.0f : 0.0f;
            else r = v.at(s.c) != 0.0f ? a : b;     // select
            v[s.dst] = r;
            ++pc;
        }
        out[i] = v.at(p.out);
    }
    return true;
}
