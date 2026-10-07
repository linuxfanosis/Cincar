"""Download AMD's machine-readable ISA spec and print the parts a decoder needs.
Usage: python3 tools/explore_isa.py     (the download lands in third_party/isa, which git ignores)"""
import os, sys, zipfile, urllib.request
import xml.etree.ElementTree as ET

URL = "https://gpuopen.com/download/machine-readable-isa/latest/"
DIR = "third_party/isa"
ZIP = os.path.join(DIR, "machine-readable-isa.zip")
os.makedirs(DIR, exist_ok=True)

def local(tag):
    return tag.split('}')[-1]

def child(el, name):
    for c in el:
        if local(c.tag) == name:
            return c
    return None

def text(el, name):
    c = child(el, name)
    return (c.text or "").strip() if c is not None else ""

def dump(el, limit):
    try:
        ET.indent(el)
    except Exception:
        pass
    s = ET.tostring(el, encoding="unicode")
    print(s[:limit] + ("\n   ... [truncated, %d chars total]" % len(s) if len(s) > limit else ""))

if not os.path.exists(ZIP):
    print("downloading", URL)
    req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0 (cincar isa explorer)"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        with open(ZIP, "wb") as f:
            f.write(data)
    except Exception as e:
        sys.exit("download failed: %s\nDownload the zip in your browser from "
                 "https://gpuopen.com/machine-readable-isa/ and save it as %s" % (e, ZIP))
print("zip size:", os.path.getsize(ZIP), "bytes")

try:
    z = zipfile.ZipFile(ZIP)
except zipfile.BadZipFile:
    with open(ZIP, "rb") as f:
        head = f.read(200)
    os.remove(ZIP)
    sys.exit("that was not a zip file (first bytes: %r). Download it in your browser and save it as %s" % (head, ZIP))

names = z.namelist()
print("files in zip:")
for n in names[:40]:
    print("  ", n)
cands = [n for n in names if n.lower().endswith(".xml") and "rdna2" in n.lower()]
if not cands:
    sys.exit("no RDNA 2 XML file found in the zip")
z.extract(cands[0], DIR)
path = os.path.join(DIR, cands[0])
print("\nparsing", path, "(%d bytes)" % os.path.getsize(path))
root = ET.parse(path).getroot()

doc = child(root, "Document")
print("\n=== Document ===")
if doc is not None:
    for c in doc:
        print(" ", local(c.tag), ":", (c.text or "").strip()[:300])
isa = child(root, "ISA")
print("\narchitecture:", text(child(isa, "Architecture"), "ArchitectureName"))

encs = child(isa, "Encodings")
print("\n=== Encodings (name, bit count) ===")
for e in encs:
    print("  ", text(e, "EncodingName"), text(e, "BitCount"))

for want, limit in (("VOP2", 5000), ("VOPC", 3500)):
    print("\n=== Encoding element: %s ===" % want)
    shown = 0
    for e in encs:
        if text(e, "EncodingName").upper().endswith(want) and shown < 1:
            dump(e, limit)
            shown += 1
    if shown == 0:
        print("  (no encoding with a name ending in %s)" % want)

insts = child(isa, "Instructions")
targets = {"V_ADD_F32", "V_CNDMASK_B32", "V_CMP_LT_F32"}
print("\n=== Instruction elements ===")
found = set()
for i in insts:
    n = text(i, "InstructionName").upper()
    if n in targets:
        found.add(n)
        print("\n--", n)
        dump(i, 3500)
for t in sorted(targets - found):
    print("\n-- %s not found by exact name; similar names:" % t)
    key = t.split("_", 1)[1]
    print("  ", [text(i, "InstructionName") for i in insts if key in text(i, "InstructionName").upper()][:12])

ots = child(isa, "OperandTypes")
print("\n=== OperandType names (first 40) ===")
print([text(o, "OperandTypeName") for o in ots][:40])
print("\ndone")
