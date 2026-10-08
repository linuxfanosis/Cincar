#include "translate.hpp"
#include <array>
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
        // A value is four floats; scalars use component 0.
        using V4 = std::array<float, 4>;
        std::map<std::string, V4> v;
        for (const auto& name : p.inputs) v[name] = V4{inputs.at(name)[i], 0, 0, 0};
        for (const auto& c : p.consts) v[c.first] = V4{c.second, 0, 0, 0};
        auto F = [&](const std::string& name) { return v.at(name)[0]; };

        size_t pc = 0, loop_start = 0;
        long iter = 0, count = 0;
        while (pc < p.insts.size()) {
            const Inst& s = p.insts[pc];
            if (s.op == "loop") { count = std::atol(s.a.c_str()); iter = 0; loop_start = pc; ++pc; continue; }
            if (s.op == "end")  { ++iter; pc = iter < count ? loop_start + 1 : pc + 1; continue; }
            if (s.op == "iter") { v[s.dst] = V4{(float)iter, 0, 0, 0}; ++pc; continue; }
            if (s.op == "var" || s.op == "set") { v[s.dst] = v.at(s.a); ++pc; continue; }
            if (s.op == "vec4") { v[s.dst] = V4{F(s.a), F(s.b), F(s.c), F(s.d)}; ++pc; continue; }
            if (s.op == "get") { v[s.dst] = V4{v.at(s.a)[s.b[0] - '0'], 0, 0, 0}; ++pc; continue; }
            if (s.op == "vadd" || s.op == "vsub" || s.op == "vmul") {
                V4 r{};
                for (int k = 0; k < 4; ++k) {
                    float x = v.at(s.a)[k], y = v.at(s.b)[k];
                    r[k] = s.op == "vadd" ? x + y : s.op == "vsub" ? x - y : x * y;
                }
                v[s.dst] = r; ++pc; continue;
            }
            if (s.op == "vscale") {
                V4 r{};
                for (int k = 0; k < 4; ++k) r[k] = v.at(s.a)[k] * F(s.b);
                v[s.dst] = r; ++pc; continue;
            }
            if (s.op == "dot") {
                float d = 0;
                for (int k = 0; k < 4; ++k) d += v.at(s.a)[k] * v.at(s.b)[k];
                v[s.dst] = V4{d, 0, 0, 0}; ++pc; continue;
            }

            float a = s.a.empty() ? 0.0f : F(s.a);
            float b = s.b.empty() ? 0.0f : F(s.b);
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
            else r = F(s.c) != 0.0f ? a : b;     // select
            v[s.dst] = V4{r, 0, 0, 0};
            ++pc;
        }
        out[i] = F(p.out);
    }
    return true;
}
