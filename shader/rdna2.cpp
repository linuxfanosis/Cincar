#include "rdna2.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace {

struct OpInfo { const char* name; const char* ir_op; int opcode; };

// ENC_VOP2: bit 31 = 0, [30:25] opcode, [24:17] VDST, [16:9] VSRC1, [8:0] SRC0
const OpInfo kVop2[] = {
    {"V_CNDMASK_B32", "select", 1},
    {"V_ADD_F32",     "add",    3},
    {"V_SUB_F32",     "sub",    4},
    {"V_MUL_F32",     "mul",    8},
    {"V_MIN_F32",     "min",   15},
    {"V_MAX_F32",     "max",   16},
};
// ENC_VOPC: [31:25] = 0b0111110, [24:17] opcode, [16:9] VSRC1, [8:0] SRC0; result goes to VCC
const OpInfo kVopc[] = {
    {"V_CMP_LT_F32",  "lt",  1},
    {"V_CMP_EQ_F32",  "eq",  2},
    {"V_CMP_LE_F32",  "le",  3},
    {"V_CMP_GT_F32",  "gt",  4},
    {"V_CMP_GE_F32",  "ge",  6},
    {"V_CMP_NEQ_F32", "ne", 13},   // !(a == b): true when unordered, like C's !=
};

constexpr uint32_t S_ENDPGM = 0xBF810000u;

// SRC0 operand codes
constexpr uint32_t SRC_ZERO = 128;                 // inline 0
constexpr uint32_t SRC_INLINE_FLOAT_FIRST = 240;   // 240..247 = 0.5 -0.5 1.0 -1.0 2.0 -2.0 4.0 -4.0
constexpr uint32_t SRC_INLINE_FLOAT_LAST = 247;
constexpr uint32_t SRC_LITERAL = 255;
constexpr uint32_t SRC_VGPR_BASE = 256;
const float kInlineFloats[8] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};

struct Decoder {
    Program& prog;
    std::map<int, int> ver;          // VGPR number -> latest version (absent = never written)
    int vcc_ver = 0;                 // 0 = VCC never written
    std::set<uint32_t> const_bits;   // literal/inline constants already declared
    std::string err;

    explicit Decoder(Program& p) : prog(p) {}

    static std::string vname(int n, int v) {
        return v == 0 ? "v" + std::to_string(n) : "v" + std::to_string(n) + "_" + std::to_string(v);
    }

    std::string add_const(float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        char name[32];
        std::snprintf(name, sizeof(name), "c_%08x", bits);
        if (const_bits.insert(bits).second) prog.consts.push_back({name, f});
        return name;
    }

    bool fail(size_t pc, uint32_t word, const std::string& msg) {
        char head[64];
        std::snprintf(head, sizeof(head), "word %zu (0x%08x): ", pc, word);
        err = head + msg;
        return false;
    }

    bool read_vgpr(int n, std::string& out, size_t pc, uint32_t word) {
        auto it = ver.find(n);
        if (it == ver.end()) return fail(pc, word, "reads v" + std::to_string(n) + " before it is written");
        out = vname(n, it->second);
        return true;
    }

    // Resolve a 9-bit SRC0 field to an IR value name. `consumed` is how many words the
    // instruction occupies (2 when a literal constant follows).
    bool read_src0(uint32_t src, const std::vector<uint32_t>& w, size_t pc, std::string& out,
                   size_t& consumed) {
        uint32_t word = w[pc];
        if (src >= SRC_VGPR_BASE) return read_vgpr((int)(src - SRC_VGPR_BASE), out, pc, word);
        if (src == SRC_ZERO) { out = add_const(0.0f); return true; }
        if (src >= SRC_INLINE_FLOAT_FIRST && src <= SRC_INLINE_FLOAT_LAST) {
            out = add_const(kInlineFloats[src - SRC_INLINE_FLOAT_FIRST]);
            return true;
        }
        if (src == SRC_LITERAL) {
            if (pc + 1 >= w.size()) return fail(pc, word, "literal constant is missing (end of program)");
            float f;
            std::memcpy(&f, &w[pc + 1], 4);
            out = add_const(f);
            consumed = 2;
            return true;
        }
        if (src == 249 || src == 250 || src == 233 || src == 234)
            return fail(pc, word, "SDWA/DPP encodings are not supported");
        return fail(pc, word, "unsupported source operand " + std::to_string(src) +
                              " (only VGPRs, inline 0, inline float constants and literals so far)");
    }
};

} // namespace

const std::vector<Rdna2Supported>& rdna2_supported() {
    static const std::vector<Rdna2Supported> t = [] {
        std::vector<Rdna2Supported> v;
        for (const auto& o : kVop2) v.push_back({o.name, "ENC_VOP2", o.opcode});
        for (const auto& o : kVopc) v.push_back({o.name, "ENC_VOPC", o.opcode});
        return v;
    }();
    return t;
}

bool decode_rdna2(const std::vector<uint32_t>& words, const Rdna2Options& opts,
                  Program& prog, std::string& error) {
    prog = Program{};
    Decoder d(prog);
    if (opts.num_inputs < 0 || opts.num_inputs > 255) { error = "number of inputs must be 0..255"; return false; }
    if (opts.out_vgpr < 0 || opts.out_vgpr > 255) { error = "an output register (--out vN) is required"; return false; }
    for (int i = 0; i < opts.num_inputs; ++i) {
        prog.inputs.push_back("v" + std::to_string(i));
        d.ver[i] = 0;
    }

    size_t pc = 0;
    while (pc < words.size()) {
        const uint32_t w = words[pc];
        if (w == S_ENDPGM) break;
        size_t consumed = 1;

        if ((w >> 25) == 0x3E) {                              // ---- VOPC ----
            int opcode = (int)((w >> 17) & 0xFF);
            const OpInfo* info = nullptr;
            for (const auto& o : kVopc) if (o.opcode == opcode) info = &o;
            if (!info) { d.fail(pc, w, "unsupported VOPC opcode " + std::to_string(opcode)); error = d.err; return false; }
            std::string a, b;
            if (!d.read_src0(w & 0x1FF, words, pc, a, consumed) ||
                !d.read_vgpr((int)((w >> 9) & 0xFF), b, pc, w)) { error = d.err; return false; }
            d.vcc_ver++;
            prog.insts.push_back({info->ir_op, "vcc_" + std::to_string(d.vcc_ver), a, b, ""});
        } else if ((w >> 25) == 0x3F) {                       // ---- VOP1 (not decoded) ----
            d.fail(pc, w, "unsupported instruction encoding VOP1 (only VOP2 and VOPC are decoded so far)");
            error = d.err; return false;
        } else if ((w >> 31) == 0) {                          // ---- VOP2 ----
            int opcode = (int)((w >> 25) & 0x3F);
            int vdst = (int)((w >> 17) & 0xFF);
            const OpInfo* info = nullptr;
            for (const auto& o : kVop2) if (o.opcode == opcode) info = &o;
            if (!info) { d.fail(pc, w, "unsupported VOP2 opcode " + std::to_string(opcode)); error = d.err; return false; }
            std::string a, b;                                 // a = SRC0, b = VSRC1
            if (!d.read_src0(w & 0x1FF, words, pc, a, consumed) ||
                !d.read_vgpr((int)((w >> 9) & 0xFF), b, pc, w)) { error = d.err; return false; }
            int newver = (d.ver.count(vdst) ? d.ver[vdst] : 0) + 1;
            std::string dst = Decoder::vname(vdst, newver);
            if (std::string(info->ir_op) == "select") {
                // D = VCC ? VSRC1 : SRC0   ->   select D, VCC, VSRC1, SRC0
                if (d.vcc_ver == 0) { d.fail(pc, w, "v_cndmask_b32 reads VCC before any compare wrote it"); error = d.err; return false; }
                prog.insts.push_back({"select", dst, b, a, "vcc_" + std::to_string(d.vcc_ver)});
            } else {
                prog.insts.push_back({info->ir_op, dst, a, b, ""});
            }
            d.ver[vdst] = newver;
        } else {
            d.fail(pc, w, "unsupported instruction encoding (only VOP2 and VOPC are decoded so far)");
            error = d.err; return false;
        }
        pc += consumed;
    }

    auto it = d.ver.find(opts.out_vgpr);
    if (it == d.ver.end()) {
        error = "output register v" + std::to_string(opts.out_vgpr) + " was never written (and is not an input)";
        return false;
    }
    prog.out = Decoder::vname(opts.out_vgpr, it->second);
    return true;
}
