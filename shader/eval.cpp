#include "translate.hpp"
#include <cmath>

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
        std::map<std::string, float> v;
        for (const auto& name : p.inputs) v[name] = inputs.at(name)[i];
        for (const auto& c : p.consts) v[c.first] = c.second;
        for (const auto& s : p.insts) {
            float a = v.at(s.a);
            float b = s.b.empty() ? 0.0f : v.at(s.b);
            float r;
            if (s.op == "add") r = a + b;
            else if (s.op == "sub") r = a - b;
            else if (s.op == "mul") r = a * b;
            else if (s.op == "div") r = a / b;
            else if (s.op == "min") r = std::fmin(a, b);
            else if (s.op == "max") r = std::fmax(a, b);
            else if (s.op == "sqrt") r = std::sqrt(a);
            else r = std::fabs(a);   // abs
            v[s.dst] = r;
        }
        out[i] = v.at(p.out);
    }
    return true;
}
