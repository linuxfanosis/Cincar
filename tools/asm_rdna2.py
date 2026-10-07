#!/usr/bin/env python3
"""Assemble RDNA 2 (gfx1030) assembly with LLVM's llvm-mc and print the instruction words.

usage: asm_rdna2.py file.s     prints one 8-digit hex dword per line
       asm_rdna2.py --check    exit 0 if a working llvm-mc with the AMDGPU target exists
Exit codes: 0 ok, 1 assembly error, 2 llvm-mc missing or has no AMDGPU support (callers skip).
Install on Ubuntu with:  sudo apt-get install -y llvm
"""
import glob, os, re, shutil, subprocess, sys

def find_llvm_mc():
    p = shutil.which("llvm-mc")
    if p:
        return p
    for pattern in ("/usr/bin/llvm-mc-*", "/usr/lib/llvm-*/bin/llvm-mc"):
        hits = sorted(glob.glob(pattern), reverse=True)
        if hits:
            return hits[0]
    return None

def assemble(mc, text):
    cmd = [mc, "-triple=amdgcn", "-mcpu=gfx1030", "-mattr=+wavefrontsize64", "-show-encoding"]
    r = subprocess.run(cmd, input=text, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr.strip() or "llvm-mc failed"
    data = []
    for line in r.stdout.splitlines():
        m = re.search(r"encoding:\s*\[([^\]]*)\]", line)
        if m:
            data += [int(b, 16) for b in m.group(1).split(",") if b.strip()]
    if not data or len(data) % 4:
        return None, "unexpected llvm-mc output (%d bytes of encoding)" % len(data)
    return ["%08x" % int.from_bytes(bytes(data[i:i + 4]), "little") for i in range(0, len(data), 4)], None

def main():
    mc = find_llvm_mc()
    if mc is None:
        print("llvm-mc not found (sudo apt-get install -y llvm)", file=sys.stderr)
        return 2
    if len(sys.argv) == 2 and sys.argv[1] == "--check":
        words, err = assemble(mc, "s_endpgm\n")
        if words != ["bf810000"]:
            print("llvm-mc cannot assemble for gfx1030: %s" % (err or words), file=sys.stderr)
            return 2
        return 0
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1
    words, err = assemble(mc, open(sys.argv[1]).read())
    if words is None:
        print("error: " + err, file=sys.stderr)
        return 1
    print("\n".join(words))
    return 0

sys.exit(main())
