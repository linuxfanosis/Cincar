#!/usr/bin/env python3
"""How much of the PS5-relevant AMD instruction set does our decoder cover?

usage: isa_coverage.py [path/to/rdna2dec]
Needs third_party/isa/amdgpu_isa_rdna2.xml, and amdgpu_isa_rdna1.xml for the full set
(python3 tools/explore_isa.py and python3 tools/isa_gens.py download and extract them).

The PS5 GPU (gfx1013) sits between the generations: it has gfx10.1-only instructions from RDNA 1 and
RDNA 2 additions such as ray-tracing and dot-product ops. The relevant set is the union of the two specs;
its size (1,166) matches the instruction count AnyPS5's README tracks.

Each instruction is grouped under the *smallest* base encoding it has (32-bit ENC_VOPC beats 64-bit
ENC_VOP3 for v_cmp_*); that grouping is ours. This counts instruction *names*; it says nothing about how
often real shaders use them, or whether an implementation is correct."""
import collections, os, subprocess, sys
import xml.etree.ElementTree as ET

DIR = "third_party/isa"
BIN = sys.argv[1] if len(sys.argv) > 1 else "build/rdna2dec"

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

def load(path):
    """-> {instruction name: family}"""
    isa = child(ET.parse(path).getroot(), "ISA")
    bits = {text(e, "EncodingName"): int(text(e, "BitCount") or 0) for e in child(isa, "Encodings")}
    out = {}
    for inst in child(isa, "Instructions"):
        encs = [text(e, "EncodingName") for e in child(inst, "InstructionEncodings")]
        base = [e for e in encs if e.startswith("ENC_") and e in bits]
        fam = min(base, key=lambda e: bits[e]) if base else (encs[0] if encs else "?")
        # variant encodings (literal / SDWA / DPP / extra-field forms) belong to their base family
        for suffix in ("_INST_LITERAL", "_VOP_SDWA", "_VOP_DPP8", "_VOP_DPP16", "_SDST_ENC", "_SDWA_SDST_ENC"):
            if fam.endswith(suffix):
                fam = fam[:-len(suffix)]
        out[text(inst, "InstructionName")] = fam.replace("ENC_", "")
    return out

p2 = os.path.join(DIR, "amdgpu_isa_rdna2.xml")
p1 = os.path.join(DIR, "amdgpu_isa_rdna1.xml")
if not os.path.exists(p2):
    print("SKIP: %s not found (run python3 tools/explore_isa.py)" % p2)
    sys.exit(77)

family = {}
if os.path.exists(p1):
    family.update(load(p1))
else:
    print("note: %s not found (run python3 tools/isa_gens.py); using RDNA 2 only" % p1)
family.update(load(p2))      # RDNA 2 grouping wins where both specs list an instruction

supported = set()
try:
    out = subprocess.run([BIN, "--list-supported"], capture_output=True, text=True, check=True).stdout
    supported = {line.split()[0] for line in out.splitlines() if line.strip()}
except Exception as e:
    print("note: could not run %s (%s); counting 0 supported" % (BIN, e))

total = collections.Counter(family.values())
have = collections.Counter(family[n] for n in supported if n in family)
unknown = sorted(n for n in supported if n not in family)

print("%-12s %6s %9s %7s" % ("family", "total", "decoded", "%"))
for fam, n in sorted(total.items(), key=lambda kv: -kv[1]):
    print("%-12s %6d %9d %6.1f%%" % (fam, n, have[fam], 100.0 * have[fam] / n))
n_total, n_have = len(family), sum(have.values())
print("%-12s %6d %9d %6.1f%%" % ("ALL", n_total, n_have, 100.0 * n_have / max(n_total, 1)))
if unknown:
    print("\nWARNING: decoder lists instructions that are not in the spec:", unknown)
    sys.exit(1)
print("\nFor scale: AnyPS5's README tracks 1,166 instructions, the size of the RDNA 1 + RDNA 2 union.")
