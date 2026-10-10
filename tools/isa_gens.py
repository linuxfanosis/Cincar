#!/usr/bin/env python3
"""Compare instruction sets across AMD's machine-readable ISA specs (RDNA 1 vs RDNA 2).
Hypothesis: the PS5's gfx1013 has gfx10.1-only instructions, so the relevant set is a union."""
import os, sys, zipfile
import xml.etree.ElementTree as ET

DIR = "third_party/isa"
ZIP = os.path.join(DIR, "machine-readable-isa.zip")
if not os.path.exists(ZIP):
    sys.exit("missing %s: run python3 tools/explore_isa.py first" % ZIP)

def names(xml_path):
    root = ET.parse(xml_path).getroot()
    out = set()
    for el in root.iter():
        if el.tag.split('}')[-1] == "InstructionName" and el.text:
            out.add(el.text.strip())
    return out

sets = {}
with zipfile.ZipFile(ZIP) as z:
    for gen in ("rdna1", "rdna2"):
        member = [n for n in z.namelist() if gen in n.lower() and n.lower().endswith(".xml")]
        if not member:
            sys.exit("no %s xml in the zip" % gen)
        z.extract(member[0], DIR)
        sets[gen] = names(os.path.join(DIR, member[0]))

a, b = sets["rdna1"], sets["rdna2"]
print("RDNA 1 instructions:", len(a))
print("RDNA 2 instructions:", len(b))
print("union              :", len(a | b), "(AnyPS5's README tracks 1,166)")
print("\nonly in RDNA 1 (%d):" % len(a - b), sorted(a - b))
print("\nonly in RDNA 2 (%d):" % len(b - a), sorted(b - a))
