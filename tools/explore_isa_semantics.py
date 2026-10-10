"""Does AMD's ISA spec contain machine-readable instruction semantics (pseudo-code)?
Prints which element types appear inside instructions, and the non-encoding parts of three samples."""
import collections, os, sys
import xml.etree.ElementTree as ET

XML = "third_party/isa/amdgpu_isa_rdna2.xml"
if not os.path.exists(XML):
    sys.exit("missing %s: run python3 tools/explore_isa.py first" % XML)

def local(tag):
    return tag.split('}')[-1]

def child(el, name):
    for c in el:
        if local(c.tag) == name:
            return c
    return None

def text(el, name):
    e = child(el, name)
    return (e.text or "").strip() if e is not None else ""

root = ET.parse(XML).getroot()
isa = child(root, "ISA")
print("children of <ISA>:", [local(c.tag) for c in isa])
print("children of <Spec>/<root>:", [local(c.tag) for c in root])

insts = child(isa, "Instructions")
tags = collections.Counter()      # element names directly inside <Instruction>
enc_tags = collections.Counter()  # element names directly inside <InstructionEncoding>
for i in insts:
    for c in i:
        tags[local(c.tag)] += 1
    for e in (child(i, "InstructionEncodings") or []):
        for c in e:
            enc_tags[local(c.tag)] += 1
print("\nelements directly inside <Instruction>   :", dict(tags))
print("elements directly inside <InstructionEncoding>:", dict(enc_tags))

def dump(el, limit):
    try:
        ET.indent(el)
    except Exception:
        pass
    s = ET.tostring(el, encoding="unicode")
    print(s[:limit] + ("\n   ... [truncated, %d chars total]" % len(s) if len(s) > limit else ""))

for want in ("V_ADD_F32", "V_CMP_LT_F32", "V_CNDMASK_B32"):
    for i in insts:
        if text(i, "InstructionName") == want:
            print("\n=== %s: everything inside <Instruction> except <InstructionEncodings> ===" % want)
            for c in i:
                if local(c.tag) != "InstructionEncodings":
                    dump(c, 2500)
            break

# anything that looks like semantics anywhere in the file?
hits = collections.Counter()
for el in root.iter():
    t = local(el.tag).lower()
    if any(k in t for k in ("operation", "pseudo", "semantic", "behavior", "behaviour", "function", "expression")):
        hits[local(el.tag)] += 1
print("\nelement names containing operation/pseudo/semantic/behavior/function/expression:", dict(hits))
print("\ndone")
