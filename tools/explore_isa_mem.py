"""Print the parts of AMD's RDNA 2 ISA spec needed to decode buffer memory instructions.
Usage: python3 tools/explore_isa_mem.py   (needs third_party/isa/amdgpu_isa_rdna2.xml; run tools/explore_isa.py first)"""
import os, sys
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

def text(el, name=None):
    e = child(el, name) if name else el
    return (e.text or "").strip() if e is not None else ""

def dump(el, limit):
    try:
        ET.indent(el)
    except Exception:
        pass
    s = ET.tostring(el, encoding="unicode")
    print(s[:limit] + ("\n   ... [truncated, %d chars total]" % len(s) if len(s) > limit else ""))

root = ET.parse(XML).getroot()
isa = child(root, "ISA")
encs = child(isa, "Encodings")

# 1. the bit layout (MicrocodeFormat) of the encodings we need; VOP2 is included as a known reference
for want in ("ENC_VOP2", "ENC_MUBUF", "ENC_SOPP"):
    print("\n=== %s : everything except the long identifier list and description ===" % want)
    for e in encs:
        if text(e, "EncodingName") == want:
            print("BitCount:", text(e, "BitCount"), " mask:", text(e, "EncodingIdentifierMask"))
            ids = child(e, "EncodingIdentifiers")
            print("identifiers (first 4 of %d):" % (len(ids) if ids is not None else 0),
                  [(i.text or "").strip() for i in list(ids)[:4]] if ids is not None else None)
            mf = child(e, "MicrocodeFormat")
            if mf is None:
                print("(no MicrocodeFormat element; children are: %s)" % [local(c.tag) for c in e])
            else:
                dump(mf, 5000)

# 2. the instructions: compact view of every encoding of the ones we care about
def compact(inst, only_encodings):
    print("\n--", text(inst, "InstructionName"), "|", text(inst, "Description")[:110])
    for e in child(inst, "InstructionEncodings"):
        en = text(e, "EncodingName")
        if en not in only_encodings:
            continue
        op = child(e, "Opcode")
        ops = []
        operands = child(e, "Operands")
        for o in (operands if operands is not None else []):
            ops.append("%s:%s:%s%s%s" % (text(o, "FieldName") or "-", text(o, "OperandType"), text(o, "OperandSize"),
                                         "/in" if o.get("Input") == "true" else "", "/out" if o.get("Output") == "true" else ""))
        print("   %s opcode=%s radix=%s operands=[%s]" % (en, (op.text or "").strip() if op is not None else "?",
                                                         op.get("Radix") if op is not None else "?", ", ".join(ops)))

insts = child(isa, "Instructions")
wanted = {"BUFFER_LOAD_DWORD", "BUFFER_STORE_DWORD", "S_ENDPGM", "V_ADD_F32"}
print("\n=== Instructions (compact) ===")
found = set()
for i in insts:
    n = text(i, "InstructionName")
    if n in wanted:
        found.add(n)
        compact(i, {"ENC_MUBUF", "ENC_SOPP", "ENC_VOP2"})
print("\nnot found:", sorted(wanted - found))
print("\nall MUBUF load/store dword-ish names:",
      sorted(text(i, "InstructionName") for i in insts
             if ("BUFFER_LOAD_DWORD" in text(i, "InstructionName") or "BUFFER_STORE_DWORD" in text(i, "InstructionName"))))
print("\ndone")
