#!/usr/bin/env python3
"""Survey reference projects cloned under third_party/ref (read-only study material).

usage: survey_refs.py [directory containing the clones]
Prints, per project: last commit, licence files, layout, size by language, tests, docs, build system
and where the shader code lives. Facts only; the clones live in a git-ignored directory and no code
from them is ever copied into this repository."""
import collections, os, subprocess, sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "third_party/ref"
SRC_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl", ".cs", ".rs", ".py", ".glsl", ".comp", ".vert", ".frag"}
SKIP_DIRS = {".git", "node_modules", "build", "out", "bin", "obj", ".vs", ".idea"}

def git(repo, *args):
    try:
        return subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return "?"

def lines_in(path):
    try:
        with open(path, "rb") as f:
            return sum(1 for _ in f)
    except OSError:
        return 0

if not os.path.isdir(ROOT):
    sys.exit("no such directory: %s (clone the projects there first)" % ROOT)

found = False
for name in sorted(os.listdir(ROOT)):
    repo = os.path.join(ROOT, name)
    if not os.path.isdir(os.path.join(repo, ".git")):
        continue
    found = True
    print("=" * 78)
    print(name)
    print("last commit :", git(repo, "log", "-1", "--format=%h %ad %s", "--date=short")[:110])

    entries = sorted(os.listdir(repo))
    dirs = [e for e in entries if os.path.isdir(os.path.join(repo, e)) and e not in SKIP_DIRS]
    files = [e for e in entries if os.path.isfile(os.path.join(repo, e))]
    print("top dirs    :", ", ".join(dirs))
    print("top files   :", ", ".join(files[:22]))
    lic = [f for f in files if f.upper().startswith(("LICENSE", "COPYING"))]
    if os.path.isdir(os.path.join(repo, "LICENSES")):
        lic += ["LICENSES/" + f for f in sorted(os.listdir(os.path.join(repo, "LICENSES")))]
    print("licence     :", ", ".join(lic) or "(none found)")

    loc, nfiles, dir_loc = collections.Counter(), collections.Counter(), collections.Counter()
    tests, shaderdirs, docs, cmake, dotnet = 0, set(), [], 0, 0
    for dp, dn, fn in os.walk(repo):
        dn[:] = [d for d in dn if d not in SKIP_DIRS]
        rel = os.path.relpath(dp, repo)
        low = rel.lower()
        if rel != "." and ("shader" in low or "recompil" in low):
            shaderdirs.add(rel)
        for f in fn:
            ext = os.path.splitext(f)[1].lower()
            if f == "CMakeLists.txt":
                cmake += 1
            if ext in (".sln", ".slnx", ".csproj"):
                dotnet += 1
            if ext == ".md" and (low.startswith("docs") or rel == "."):
                docs.append(os.path.join(rel, f) if rel != "." else f)
            if ext in SRC_EXT:
                n = lines_in(os.path.join(dp, f))
                loc[ext] += n
                nfiles[ext] += 1
                key = "/".join(rel.split(os.sep)[:2]) if rel != "." else "."
                dir_loc[key] += n
                if "test" in low or "test" in f.lower():
                    tests += 1

    print("code (lines):", ", ".join("%s %d (%d files)" % (e, n, nfiles[e]) for e, n in loc.most_common(6)))
    print("biggest dirs:", ", ".join("%s %d" % (d, n) for d, n in dir_loc.most_common(8)))
    print("test files  :", tests)
    print("build system: %d CMakeLists.txt, %d .sln/.slnx/.csproj files" % (cmake, dotnet))
    print("shader paths:", ", ".join(sorted(shaderdirs, key=lambda p: (p.count(os.sep), p))[:10]) or "(none)")
    print("docs (.md)  :", ", ".join(sorted(docs)[:18]) or "(none)")
if not found:
    print("no git clones found in %s" % ROOT)
