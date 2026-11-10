#!/usr/bin/env python3
"""Find the std::map/std::set instantiations (VC6 Dinkumware <xtree>) in
DungeonSiege.exe and name their member functions.

Every _Tree<...> instantiation has its own static _Nil node and _Nilrefs
count. _Tree::_Init is the one function that tests _Nil under std::_Lockit
and allocates it, which gives both statics; every function that reads them
belongs to that instantiation. The members are then told apart by what they
call, following <xtree>:

  _Init, ~_Tree        lock and take or drop a _Nilrefs reference
  erase(iterator)      locks, and calls _Lrotate/_Rrotate (leaves, three
                       times each) and iterator::_Inc (_P++)
  _Insert              allocates a node and calls the rotations
  _Erase, _Copy        recursive: frees / allocates nodes
  insert(value)        calls _Insert and iterator::_Dec (--_P)
  erase(iterator,iterator)  calls _Erase and erase(iterator)
  _Lbound, _Ubound     walk from the root; the compare's first operand is
                       the node's key in _Lbound, the argument in _Ubound
  clear, find, lower_bound, upper_bound, equal_range, erase(key) and the
  copy constructor call only the above (and the tree's key compare).

Inputs are what the README's steps produce: symbols/functions.tsv
(function extents), generated/table.cpp (the lifter's entries, which add
functions the analysis missed) and llvm-objdump's disassembly of the EXE.

Usage: python3 -I tools/stl_trees.py --exe DungeonSiege.exe [--out out/stl]
           [--objdump llvm-objdump]
writes OUT/members.tsv (one row per function), OUT/trees.tsv (one row per
instantiation) and OUT/names.tsv (address, name, evidence) for
tools/ghidra/ApplyNames.java (FINDINGS.md, "STL").
"""

import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

LOCKIT = 0x637710        # std::_Lockit::_Lockit
NEW, DELETE = 0x403B80, 0x403BA0   # the game's operator new / delete
TEXT_END = 0x401000 + 0x31EABA


class Image:
    def __init__(self, exe, symbols, table, objdump):
        ext = []
        for line in open(symbols):
            p = line.split("\t")
            if p[0] == "entry":
                continue
            ext.append((int(p[1], 16), int(p[2], 16) + 1, int(p[0], 16)))
        lifted = sorted({int(m, 16) for m in re.findall(r"void f_([0-9A-F]{8})\(",
                                                          open(table).read())})
        ext.sort()
        starts = [a for a, _, _ in ext]

        def covered(x):
            i = bisect.bisect_right(starts, x) - 1
            return i >= 0 and ext[i][0] <= x < ext[i][1]

        known = {e for _, _, e in ext}
        bounds = sorted(set(starts) | set(lifted))
        for e in lifted:
            if e not in known and not covered(e):
                i = bisect.bisect_right(bounds, e)
                ext.append((e, bounds[i] if i < len(bounds) else TEXT_END, e))
        ext.sort()
        self.ext = ext
        self.funcs = {e: (a, b) for a, b, e in ext}

        self.ins = {}
        rx = re.compile(r"^ +([0-9a-f]+):[ 0-9a-f]+\t(.*)$")
        dis = subprocess.run([objdump, "-d", "--x86-asm-syntax=intel", exe],
                             capture_output=True, text=True, check=True).stdout
        for line in dis.splitlines():
            m = rx.match(line)
            if m:
                self.ins[int(m.group(1), 16)] = re.sub(r"\s*<.*", "", m.group(2)).rstrip()
        self.addrs = sorted(self.ins)

        # calls, attributed to the innermost function containing the site
        self.callees = collections.defaultdict(list)
        self.callers = collections.defaultdict(set)
        estarts = [a for a, _, _ in ext]
        for a, t in self.ins.items():
            m = re.match(r"call\t0x([0-9a-f]+)$", t)
            if not m:
                continue
            i = bisect.bisect_right(estarts, a) - 1
            while i >= 0 and not (ext[i][0] <= a < ext[i][1]):
                i -= 1
            if i < 0:
                continue
            f = ext[i][2]
            self.callees[f].append(int(m.group(1), 16))
            self.callers[int(m.group(1), 16)].add(f)

        self.refs = collections.defaultdict(set)   # global -> readers
        mem = re.compile(r"\[0x([0-9a-f]{6})\]")
        for e in self.funcs:
            for t in self.text(e):
                for m in mem.finditer(t):
                    self.refs[int(m.group(1), 16)].add(e)

    def text(self, e):
        a0, a1 = self.funcs[e]
        i = bisect.bisect_left(self.addrs, a0)
        r = []
        while i < len(self.addrs) and self.addrs[i] < a1:
            r.append(self.ins[self.addrs[i]])
            i += 1
        return r

    def size(self, e):
        a0, a1 = self.funcs[e]
        return a1 - a0


# why each role was given (the plate comment's evidence)
EVIDENCE = {
    "_Init": "tests _Nil under _Lockit, allocates it and the head, ++_Nilrefs",
    "~_Tree": "clear(), frees the head, --_Nilrefs under _Lockit, frees _Nil at 0",
    "erase(iterator)": "_P++, then unlinks under _Lockit with the delete fix-up rotations",
    "_Lrotate": "leaf, _Y = _Right(_X) first",
    "_Rrotate": "leaf, _Y = _Left(_X) first",
    "iterator::_Inc": "successor walk (_Right, then up while a right child)",
    "iterator::_Dec": "the iterator step insert(value) takes before its last _Insert",
    "_Insert": "allocates a node and rebalances with the rotations, no _Lockit",
    "_Erase": "recursive, frees nodes",
    "_Copy": "recursive, allocates nodes",
    "_Copy(tree)": "calls only _Copy",
    "insert(value)": "calls _Insert (two or more sites) and iterator::_Dec",
    "insert(iterator,value)": "hint insert: calls _Insert and insert(value)",
    "erase(iterator,iterator)": "calls _Erase and erase(iterator)",
    "_Lbound": "walks from the root keeping nodes with !key_compare(_Key(_X), _Kv)",
    "_Ubound": "walks from the root keeping nodes with key_compare(_Kv, _Key(_X))",
    "clear()": "erase(begin(), end()) only",
    "lower_bound(key)": "_Lbound only",
    "upper_bound(key)": "_Ubound only",
    "find(key)": "_Lbound, then end() unless the key compares equal",
    "equal_range(key)": "_Lbound and _Ubound into a pair",
    "erase(key)": "equal_range, counts with _Inc, erase(iterator,iterator)",
    "_Tree(const&)": "copies comparator/allocator/_Multi, _Init, _Copy(tree)",
    "?": "unclassified",
}


def bound(b):
    """_Lbound or _Ubound, from the first comparison of the walk.

    The node's key is addressed off the node register ([ecx/esi + n]); the
    argument comes from the stack. key_compare(_Key(_X), _Kv) is _Lbound,
    key_compare(_Kv, _Key(_X)) _Ubound."""
    key = re.compile(r"\[e(?:cx|si) \+ 0x[0-9a-f]+\]")
    for i, t in enumerate(b):
        op, _, args = t.partition("\t")
        if op == "cmp" and "byte ptr" in args:    # inline strcmp, key first
            return "_Lbound"
        if op == "cmp" and args in ("edx, edi", "edi, edx"):
            return "_Lbound" if args == "edx, edi" else "_Ubound"
        if op == "cmp" and key.search(args):
            return "_Lbound" if key.search(args.split(", ")[0]) else "_Ubound"
        if op == "fld":
            return "_Lbound" if key.search(args) else "_Ubound"
        if op == "call":
            if b[i - 1].startswith("lea\tecx, [esi"):  # _Key(_X).operator<
                return "_Lbound"
            last = next(x for x in reversed(b[:i]) if x.startswith("push"))
            return "_Ubound" if "esp +" in last else "_Lbound"
    return None


def key_kind(img, e, members):
    """How a tree compares keys: the first comparison in e's body."""
    b = img.text(e)
    for i, t in enumerate(b):
        op, _, a = t.partition("\t")
        if op == "call":
            tgt = int(a, 16)
            if tgt in members or tgt in (NEW, DELETE, LOCKIT):
                continue
            return f"call {tgt:08X}"
        if op == "fld":
            return "float" if a.startswith("dword") else "double"
        if op != "cmp":
            continue
        if "byte ptr" in a:
            return "char* (strcmp)"
        nxt = b[i + 1].split("\t")[0]
        if re.match(r"(word ptr|dx,|si,|cx,)", a) and not a.startswith("dword"):
            return "uint16"
        if re.search(r"\[e(?:cx|si|ax) \+ 0x|^e[a-d]x, e[ds]i$|^e[ds]i, e[a-d]x$", a):
            two = any(x.startswith("cmp\tdx, word") or x.startswith("cmp\tcx, word") for x in b)
            base = "uint32" if nxt in ("jae", "jb", "jbe", "ja") else \
                   "int32" if nxt in ("jge", "jl", "jle", "jg") else "uint32"
            return base + ("+uint16" if two else "")
    return None


def classify(img):
    trees = []
    for init in sorted(img.callers[LOCKIT]):
        b = img.text(init)
        if img.size(init) != 137 or NEW not in img.callees[init]:
            continue
        nil = next((int(m.group(1), 16) for t in b
                    if (m := re.match(r"cmp\tdword ptr \[0x([0-9a-f]+)\], ebx$", t))), None)
        nilrefs = next((int(m.group(1), 16) for t in b
                        if (m := re.match(r"inc\tdword ptr \[0x([0-9a-f]+)\]$", t))), None)
        node = next((int(m.group(1), 16) for t in b
                     if (m := re.match(r"push\t0x([0-9a-f]+)$", t))), None)
        if None in (nil, nilrefs, node):
            continue
        trees.append({"init": init, "nil": nil, "nilrefs": nilrefs, "node": node,
                      "members": img.refs[nil] | img.refs[nilrefs]})

    role, tree_of = {}, {}
    for t in trees:
        ms = t["members"]
        r = {t["init"]: "_Init"}
        cs = img.callees
        for m in ms:
            if m != t["init"] and LOCKIT in cs[m] and any(
                    x == f"dec\tdword ptr [0x{t['nilrefs']:x}]" for x in img.text(m)):
                r[m] = "~_Tree"
        for m in ms - r.keys():
            leaf = collections.Counter(c for c in cs[m] if c in ms and not cs[c])
            rot = [c for c, n in leaf.items() if n >= 3]
            if LOCKIT in cs[m] and len(rot) == 2:
                r[m] = "erase(iterator)"
                inc = next((c for c in cs[m] if c in ms and c not in rot), None)
                if inc is not None:
                    r.setdefault(inc, "iterator::_Inc")
        # rotations by their first loads: _Lrotate takes _Right(_X) (node+8)
        for m in ms - r.keys():
            b = img.text(m)
            if not cs[m] and len(b) > 2 and b[0] == "mov\tedx, dword ptr [esp + 0x4]":
                if b[1].endswith("[edx + 0x8]"):
                    r[m] = "_Lrotate"
                elif b[1].endswith("[edx]"):
                    r[m] = "_Rrotate"
        rots = {m for m, x in r.items() if x in ("_Lrotate", "_Rrotate")}
        for m in ms - r.keys():
            if NEW in cs[m] and rots & set(cs[m]) and LOCKIT not in cs[m]:
                r[m] = "_Insert"
            elif m in cs[m] and DELETE in cs[m] and NEW not in cs[m]:
                r[m] = "_Erase"
            elif m in cs[m] and NEW in cs[m]:
                r[m] = "_Copy"
        of = lambda name: {m for m, x in r.items() if x == name}
        for m in ms - r.keys():
            c = set(cs[m])
            if len([x for x in cs[m] if x in of("_Insert")]) >= 2:
                r[m] = "insert(value)"
                for x in cs[m]:
                    if x in ms and x not in of("_Insert"):
                        r.setdefault(x, "iterator::_Dec")
            elif c & of("erase(iterator)") and c & of("_Erase"):
                r[m] = "erase(iterator,iterator)"
        for m in ms - r.keys():
            c = set(cs[m])
            if c & of("_Insert") and c & of("insert(value)"):
                r[m] = "insert(iterator,value)"
            elif c and c <= of("_Copy"):
                r[m] = "_Copy(tree)"
            elif img.text(m)[:2] == ["mov\tedx, dword ptr [ecx]", "mov\teax, dword ptr [edx + 0x8]"]:
                r[m] = "iterator::_Inc"
            elif all(x not in ms and x not in (NEW, DELETE, LOCKIT) for x in cs[m]):
                r[m] = bound(img.text(m)) or "?"
        for m in ms:
            role[m] = r.get(m, "?")
            tree_of[m] = t["nil"]

    # the thin members above them: only tree members and the key compare
    cmp_of = collections.defaultdict(set)
    for m, x in role.items():
        if x in ("_Lbound", "_Ubound"):
            cmp_of[tree_of[m]] |= set(img.callees[m])
    cands = set().union(*(img.callers[m] for m in role)) - role.keys()
    for c in sorted(cands):
        cs = img.callees[c]
        ts = {tree_of[x] for x in cs if x in role}
        if len(ts) != 1:
            continue
        t = ts.pop()
        if any(x not in role and x not in cmp_of[t] for x in cs):
            continue
        rs = collections.Counter(role[x] for x in cs if x in role)
        n = img.size(c)
        x = None
        if rs == {"erase(iterator,iterator)": 1} and n <= 24:
            x = "clear()"
        elif rs == {"_Lbound": 1} and n <= 24:
            x = "lower_bound(key)"
        elif rs == {"_Ubound": 1} and n <= 24:
            x = "upper_bound(key)"
        elif rs == {"_Lbound": 1} and n <= 110 and "ret\t0x8" in img.text(c):
            x = "find(key)"
        elif rs == {"_Lbound": 1, "_Ubound": 1} and n <= 60:
            x = "equal_range(key)"
        elif set(rs) == {"_Lbound", "_Ubound", "erase(iterator,iterator)", "iterator::_Inc"} and n <= 100:
            x = "erase(key)"
        elif set(rs) == {"_Init", "_Copy(tree)"} and n <= 80:
            x = "_Tree(const&)"
        if x:
            role[c], tree_of[c] = x, t

    for t in trees:
        ms = {m for m in role if tree_of[m] == t["nil"]}
        t["members"] = ms
        src = [m for m in ms if role[m] in ("_Lbound", "_Ubound")] or \
              [m for m in ms if role[m] == "insert(value)"]
        t["key"] = next((k for m in sorted(src) if (k := key_kind(img, m, ms))), None)
    return trees, role, tree_of


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--symbols", default="symbols/functions.tsv")
    ap.add_argument("--table", default="generated/table.cpp")
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--out", default="out/stl")
    a = ap.parse_args()
    img = Image(a.exe, a.symbols, a.table, a.objdump)
    trees, role, tree_of = classify(img)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "members.tsv"), "w") as o:
        o.write("va\tnil\trole\tsize\n")
        for m in sorted(role):
            o.write(f"{m:08X}\t{tree_of[m]:08X}\t{role[m]}\t{img.size(m)}\n")
    with open(os.path.join(a.out, "trees.tsv"), "w") as o:
        o.write("nil\tnilrefs\tnode\tkey\tmembers\n")
        for t in trees:
            o.write(f"{t['nil']:08X}\t{t['nilrefs']:08X}\t{t['node']:#x}\t"
                    f"{t['key'] or '-'}\t{len(t['members'])}\n")
    stats = collections.Counter(role.values())
    print(f"{len(trees)} instantiations, {len(role)} functions", file=sys.stderr)
    for r, n in stats.most_common():
        print(f"{n:5d} {r}", file=sys.stderr)
    tinfo = {t["nil"]: t for t in trees}
    with open(os.path.join(a.out, "names.tsv"), "w") as o:
        for m in sorted(role):
            t = tinfo[tree_of[m]]
            o.write(f"{m:08X}\tstd::_Tree@{t['nil']:X}::{role[m]}\t"
                    f"VC6 <xtree> _Tree member {role[m]}: {EVIDENCE[role[m]]}. "
                    f"Instantiation: _Nil {t['nil']:#x}, _Nilrefs {t['nilrefs']:#x}, "
                    f"node {t['node']:#x} bytes, key {t['key'] or 'not compared'}. "
                    f"From tools/stl_trees.py.\n")


if __name__ == "__main__":
    main()
