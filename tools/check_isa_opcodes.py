#!/usr/bin/env python3
"""Check the decoder's instruction table against AMD's machine-readable RDNA 2 spec.

usage: check_isa_opcodes.py [path/to/rdna2dec]
Needs third_party/isa/amdgpu_isa_rdna2.xml (run tools/explore_isa.py once to download it).
Exit codes: 0 all match, 1 mismatch, 77 spec not downloaded (callers skip)."""
import os, subprocess, sys
import xml.etree.ElementTree as ET

XML = "third_party/isa/amdgpu_isa_rdna2.xml"
BIN = sys.argv[1] if len(sys.argv) > 1 else "build/rdna2dec"

def local(tag):
    return tag.split('}')[-1]

def child(el, name):
    for c in el:
        if local(c.tag) == name:
            return c
    return None

def text(el):
    return (el.text or "").strip() if el is not None else ""

if not os.path.exists(XML):
    print("SKIP: %s not found" % XML)
    sys.exit(77)

listing = subprocess.run([BIN, "--list-supported"], capture_output=True, text=True, check=True).stdout
wanted = []
for line in listing.splitlines():
    name, enc, opc = line.split()
    wanted.append((name, enc, int(opc)))

root = ET.parse(XML).getroot()
insts = child(child(root, "ISA"), "Instructions")
by_name = {text(child(i, "InstructionName")): i for i in insts}

bad = 0
for name, enc, opc in wanted:
    inst = by_name.get(name)
    if inst is None:
        print("MISSING     %s is not in the spec" % name)
        bad += 1
        continue
    found = None
    for e in child(inst, "InstructionEncodings"):
        if text(child(e, "EncodingName")) == enc:
            op = child(e, "Opcode")
            found = int(text(op), int(op.get("Radix", "10")))
    if found is None:
        print("NO-ENCODING %s has no %s encoding in the spec" % (name, enc))
        bad += 1
    elif found != opc:
        print("MISMATCH    %s %s: decoder says opcode %d, spec says %d" % (name, enc, opc, found))
        bad += 1
    else:
        print("ok          %s %s opcode %d" % (name, enc, opc))
sys.exit(1 if bad else 0)
