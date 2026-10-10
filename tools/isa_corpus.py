#!/usr/bin/env python3
"""Build a ground-truth instruction corpus from AMD's specs, checked by LLVM's disassembler.

usage: isa_corpus.py [--out third_party/isa/corpus.tsv] [--jobs 4] [--targets gfx1013,gfx1030]
For every instruction in the RDNA 1 and RDNA 2 specs and every base (ENC_*) encoding it has (or, failing that, its
first listed encoding), this builds one instruction word from the spec's own data: the encoding's identifier bits
with the OP field set to the opcode. All other fields stay zero, except a small documented fixup table.
LLVM then disassembles the word, trying the targets in order (the first is the PS5 GPU, gfx1013).

  VERIFIED      LLVM's mnemonic equals the spec's name: two independent sources agree on encoding and opcode
  VERIFIED_ALT  same, but only on a later target (the instruction is not accepted for the first one)
  MISMATCH      LLVM decodes the word as a different instruction
  REJECTED      LLVM refuses it on every target (often: zeroed operand fields are not valid for that instruction)
  SKIPPED       the spec lacks what we need to build a word

Output is tab-separated: name, encoding, words (hex, little-endian dwords), status, LLVM text.
Needs third_party/isa/amdgpu_isa_rdna{1,2}.xml (tools/explore_isa.py, tools/isa_gens.py) and llvm-mc."""
import argparse, collections, concurrent.futures, glob, os, re, shutil, subprocess, sys
import xml.etree.ElementTree as ET

DIR = "third_party/isa"

# Values some words need before LLVM accepts them. Each entry is an inference to confirm by the results:
FIELD_FIXUPS = {
    "ENC_FLAT": {"SADDR": 0x7D},                       # plain FLAT has no scalar address: it must be "off"
}
NAME_FIXUPS = [
    (("DS_GWS_", "DS_ORDERED_COUNT"), {"GDS": 1}),     # global-wave-sync instructions only exist with the GDS bit
]

def local(tag):
    return tag.split('}')[-1]

def child(el, name):
    for c in el:
        if local(c.tag) == name:
            return c
    return None

def text(el, name=None):
    e = child(el, name) if name else el
    return (e.text or "").strip() if e is not None else ""

def find_llvm_mc():
    p = shutil.which("llvm-mc")
    if p:
        return p
    for pattern in ("/usr/bin/llvm-mc-*", "/usr/lib/llvm-*/bin/llvm-mc"):
        hits = sorted(glob.glob(pattern), reverse=True)
        if hits:
            return hits[0]
    return None

def load(path):
    """-> (encodings, instructions). encodings[name] = (bitcount, first identifier int, {field: [(offset, bits)]});
    instructions[name] = [(encoding, opcode)] in the spec's order"""
    isa = child(ET.parse(path).getroot(), "ISA")
    encodings = {}
    for e in child(isa, "Encodings"):
        name = text(e, "EncodingName")
        ids = child(e, "EncodingIdentifiers")
        first = None
        if ids is not None and len(ids):
            first = int((ids[0].text or "0").strip(), int(ids[0].get("Radix", "2")))
        fields = {}
        mf = child(e, "MicrocodeFormat")
        bitmap = child(mf, "BitMap") if mf is not None else None
        for f in (bitmap if bitmap is not None else []):
            layout = child(f, "BitLayout")
            if layout is None:
                continue
            rs = sorted(layout, key=lambda r: int(r.get("Order", "0")))
            fields[text(f, "FieldName")] = [(int(text(r, "BitOffset")), int(text(r, "BitCount"))) for r in rs]
        encodings[name] = (int(text(e, "BitCount") or 0), first, fields)
    instructions = {}
    for inst in child(isa, "Instructions"):
        lst = []
        for e in child(inst, "InstructionEncodings"):
            op = child(e, "Opcode")
            if op is not None:
                lst.append((text(e, "EncodingName"), int(text(op), int(op.get("Radix", "10")))))
        instructions[text(inst, "InstructionName")] = lst
    return encodings, instructions

def place(word, ranges, value):
    """Write `value` into a field made of (offset, bits) ranges, least-significant part first."""
    consumed = 0
    for off, n in ranges:
        word &= ~(((1 << n) - 1) << off)
        word |= ((value >> consumed) & ((1 << n) - 1)) << off
        consumed += n
    return word, value >> consumed == 0

def build_word(enc, opcode, fixups):
    """-> (list of little-endian dwords, None) or (None, reason)"""
    bits, ident, fields = enc
    if ident is None:
        return None, "no encoding identifier in the spec"
    if "OP" not in fields:
        return None, "no OP field in the spec"
    word, fits = place(ident, fields["OP"], opcode)
    if not fits:
        return None, "opcode %d does not fit the OP field" % opcode
    for field, value in fixups.items():
        if field in fields:
            word, _ = place(word, fields[field], value)
    return [(word >> (32 * i)) & 0xFFFFFFFF for i in range(max(bits // 32, 1))], None

SUFFIX = re.compile(r"(_e32|_e64|_dpp|_sdwa|_dpp8|_dpp16)+$")

def disassemble(mc, target, dwords):
    """-> instruction text, or None if LLVM refuses the word (it prints v_illegal for those)"""
    data = ",".join("0x%02x" % b for d in dwords for b in d.to_bytes(4, "little"))
    cmd = [mc, "--disassemble", "-triple=amdgcn", "-mcpu=" + target, "-mattr=+wavefrontsize64"]
    r = subprocess.run(cmd, input=data + "\n", capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith((".", "//", ";"))]
    if not lines or lines[0].split()[0] == "v_illegal" or "<unknown>" in lines[0]:
        return None
    return lines[0]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(DIR, "corpus.tsv"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--targets", default="gfx1013,gfx1030")
    args = ap.parse_args()
    targets = args.targets.split(",")

    p2 = os.path.join(DIR, "amdgpu_isa_rdna2.xml")
    p1 = os.path.join(DIR, "amdgpu_isa_rdna1.xml")
    if not os.path.exists(p2):
        print("SKIP: %s not found (run python3 tools/explore_isa.py)" % p2)
        return 77
    mc = find_llvm_mc()
    if mc is None:
        print("llvm-mc not found (sudo apt-get install -y llvm)")
        return 2

    encodings, instructions = {}, {}
    for p in (p1, p2):                  # RDNA 2 data wins where both specs describe the same thing
        if os.path.exists(p):
            e, i = load(p)
            encodings.update(e)
            for name, lst in i.items():
                instructions[name] = lst or instructions.get(name, [])

    jobs, rows = [], []
    for name in sorted(instructions):
        lst = instructions[name]
        cands = [x for x in lst if x[0].startswith("ENC_")] or lst[:1]
        if not cands:
            rows.append((name, "-", "", "SKIPPED", "the spec lists no encoding"))
        fix_by_name = {}
        for prefixes, f in NAME_FIXUPS:
            if name.startswith(prefixes):
                fix_by_name.update(f)
        for en, opcode in cands:
            if en not in encodings:
                rows.append((name, en, "", "SKIPPED", "encoding not described in the spec"))
                continue
            fixups = dict(FIELD_FIXUPS.get(en, {}))
            fixups.update(fix_by_name)
            dwords, why = build_word(encodings[en], opcode, fixups)
            if dwords is None:
                rows.append((name, en, "", "SKIPPED", why))
            else:
                jobs.append((name, en, dwords))

    def run(job):
        name, en, dwords = job
        words = " ".join("%08x" % d for d in dwords)
        first_other = None
        for i, target in enumerate(targets):
            out = disassemble(mc, target, dwords)
            if out is None:
                continue
            mnemonic = SUFFIX.sub("", out.split()[0].lower())
            if mnemonic == name.lower():
                return (name, en, words, "VERIFIED" if i == 0 else "VERIFIED_ALT", ("[%s] " % target if i else "") + out)
            if first_other is None:
                first_other = "[%s] %s" % (target, out)
        if first_other:
            return (name, en, words, "MISMATCH", first_other)
        return (name, en, words, "REJECTED", "")

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        rows += list(pool.map(run, jobs))
    rows.sort()
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        f.write("name\tencoding\twords\tstatus\tllvm\n")
        for r in rows:
            f.write("\t".join(r) + "\n")

    status = collections.Counter(r[3] for r in rows)
    ok_names = {r[0] for r in rows if r[3] in ("VERIFIED", "VERIFIED_ALT")}
    print("instructions: %d, words built: %d (targets tried in order: %s)" % (len(instructions), len(jobs), ", ".join(targets)))
    for k in ("VERIFIED", "VERIFIED_ALT", "MISMATCH", "REJECTED", "SKIPPED"):
        print("%-13s %5d" % (k, status[k]))
    print("instructions with at least one verified word: %d of %d" % (len(ok_names), len(instructions)))
    print("corpus written to", args.out)
    for label in ("MISMATCH", "REJECTED", "SKIPPED", "VERIFIED_ALT"):
        sel = [r for r in rows if r[3] == label]
        if not sel:
            continue
        print("\n%s (%d), grouped by encoding:" % (label, len(sel)))
        groups = collections.defaultdict(list)
        for r in sel:
            groups[r[1]].append(r[0] if label != "MISMATCH" else "%s->%s" % (r[0], r[4]))
        for en, names in sorted(groups.items()):
            print("  %s (%d): %s" % (en, len(names), ", ".join(names)))
    return 0

sys.exit(main())
