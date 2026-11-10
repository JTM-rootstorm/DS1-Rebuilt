#!/usr/bin/env python3
"""Name the STL template code in DungeonSiege.exe (FINDINGS.md, "STL").

The library is Dinkumware's for VC6, with a later <deque> (_Map, _Mapsize,
_Myoff, _Mysize) and a hint insert in <xtree>. Template code is compiled
into the game, once per type (identical bodies folded by the linker, so a
list's _Buynode is shared by every list with that node size).

std::map/std::set (<xtree>): every _Tree<...> has its own static _Nil node
and _Nilrefs count. _Tree::_Init is the one function that tests _Nil under
std::_Lockit and allocates it, which gives both statics; every function
that reads them belongs to that instantiation. The members are then told
apart by what they call:

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

std::list: _Buynode by its shape (allocate, _Next/_Prev to the arguments
or the node itself); the constructor and insert(iterator,value) by their
calls to it. std::vector: the two inserts that grow it (insert(P,n,x) and
insert(P,first,last)) start by comparing _End - _Last with the count and
allocate and free; insert(iterator,value) and push_back call insert(P,1,x). The one
deque, std::string and <stdexcept> copies were read by hand (HAND).

Inputs are what the README's steps produce: symbols/functions.tsv
(function extents), generated/table.cpp (the lifter's entries, which add
functions the analysis missed) and llvm-objdump's disassembly of the EXE.

Usage: python3 -I tools/stl_names.py --exe DungeonSiege.exe [--out out/stl]
           [--objdump llvm-objdump]
writes OUT/names.tsv (address, name, evidence) for
tools/ghidra/ApplyNames.java and OUT/trees.tsv (one row per map/set).
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


def classify_trees(img):
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


def classify_lists(img, taken):
    """list::_Buynode by shape; list() and insert(iterator,value) by their
    calls to it. Returns {va: (name, evidence)}."""
    out = {}
    bn = {}
    for e in img.funcs:
        if e in taken or img.callees[e] != [NEW]:
            continue
        b = img.text(e)
        if len(b) == 14 and b[1] == f"call\t0x{NEW:x}" and \
                b[3] == "mov\tecx, dword ptr [esp + 0x4]" and \
                b[-2] == "mov\tdword ptr [eax + 0x4], ecx" and b[-1] == "ret\t0x8":
            bn[e] = int(b[0].split("0x")[1], 16)
    for e, node in bn.items():
        out[e] = (f"std::list@node{node:X}::_Buynode",
                  f"allocates a {node:#x}-byte node, _Next/_Prev to the arguments "
                  f"or the node itself")
    for e, node in bn.items():
        for c in img.callers[e]:
            if c in taken or c in out:
                continue
            b, cs = img.text(c), img.callees[c]
            others = [x for x in cs if x != e]
            if cs == [e] and img.size(c) <= 40 and \
                    "mov\tdword ptr [esi + 0x4], eax" in b and \
                    any(re.match(r"(and|mov)\tdword ptr \[esi \+ 0x8\], 0x0$", t) for t in b):
                out[c] = (f"std::list@node{node:X}::list()",
                          "_Head = _Buynode(), _Size = 0")
            elif len(cs) - len(others) == 1 and len(set(others)) <= 1 and \
                    img.size(c) <= 160 and "ret\t0xc" in b and \
                    any(re.match(r"inc\tdword ptr \[e.. \+ 0x8\]$", t) for t in b) and \
                    any(re.match(r"push\tdword ptr \[e.. \+ 0x4\]$", t) for t in b):
                out[c] = (f"std::list@node{node:X}::insert(iterator,value)",
                          "_Buynode(_P, _Prev(_P)), links it, copies the value, ++_Size")
    return out


def classify_vectors(img, taken):
    """vector::insert(P,n,x) / insert(P,first,last), and insert(P,x) above
    them. Returns {va: (name, evidence)}."""
    out = {}
    load = re.compile(r"mov\t(e..), dword ptr \[(e(?:si|di|bx|cx)) \+ 0x(8|c)\]$")
    for e in img.funcs:
        if e in taken or NEW not in img.callees[e] or DELETE not in img.callees[e]:
            continue
        b = img.text(e)
        if "ret\t0xc" not in b:
            continue
        head = b[:20]
        regs, base = {}, None
        for t in head:
            m = load.match(t)
            if m and base in (None, m.group(2)):
                regs[m.group(3)], base = m.group(1), m.group(2)
        if set(regs) != {"8", "c"}:
            continue
        if not any(t == f"sub\t{regs['c']}, {regs['8']}" for t in head):
            continue
        # element size: from the division of the free space, if any
        elem = 1
        for i, t in enumerate(head):
            m = re.match(r"sar\t" + regs["c"] + r", 0x([0-9a-f]+)$", t)
            if m:
                elem = 1 << int(m.group(1), 16)
                break
            if t.startswith("idiv"):
                # the divisor: the last constant pushed or moved before it
                imm = re.compile(r"(?:push\t|mov\te.., )0x([0-9a-f]+)$")
                m = next((imm.match(x) for x in reversed(head[:i]) if imm.match(x)), None)
                elem = int(m.group(1), 16) if m else None
                break
        rng = any(re.match(r"sub\te.., dword ptr \[ebp \+ 0xc\]$", t) for t in head[:8])
        what = "insert(iterator,first,last)" if rng else "insert(iterator,n,value)"
        tag = f"elem{elem:X}" if elem else "elem?"
        out[e] = (f"std::vector@{tag}@{e:X}::{what}",
                  "compares _End - _Last with the count, else grows to "
                  "size() + max(size(), count): allocate, copy, fill, free")
    for e in list(out):
        if not out[e][0].endswith("insert(iterator,n,value)"):
            continue
        base = out[e][0].rsplit("::", 1)[0]
        for c in img.callers[e]:
            b = img.text(c)
            if c in taken or c in out or img.callees[c] != [e] or img.size(c) > 80:
                continue
            if "push\t0x1" not in b:
                continue
            if b[-1] == "ret\t0x4" and any(re.match(r"push\tdword ptr \[e.. \+ 0x8\]$", t) for t in b):
                out[c] = (f"{base}::push_back(value)", "insert(end(), 1, _X)")
            elif b[-1] == "ret\t0x8":
                out[c] = (f"{base}::insert(iterator,value)",
                          "insert(_P, 1, _X), returns begin() + the old offset")
    return out


# Read by hand (one copy each). The game's std::string is used only by the
# <stdexcept> classes; the deque holds 0xA0-byte elements, 25 to a block.
HAND = {
    0x6377C1: ("std::_Xlen", "throws length_error(\"string too long\")"),
    0x63781B: ("std::_Xran", "throws out_of_range(\"invalid string position\")"),
    0x5FDFDC: ("std::string::_Tidy(bool)", "drops the reference byte at _Ptr[-1] (0 or 255: frees), zeroes _Ptr/_Len/_Res"),
    0x5FE017: ("std::string::assign(const_char*,n)", "_Grow(n, true), copies, terminates"),
    0x5FDEFA: ("std::string::assign(const_string&,pos,n)", "_Xran past the end, erase() on itself, shares when whole, else _Grow and copy"),
    0x5FE056: ("std::string::erase(pos,n)", "_Xran past the end, _Split, memmove the tail, _Grow(len, false)"),
    0x5FE0BD: ("std::string::_Grow(n,trim)", "_Xlen above npos-1, unshares, trims small buffers, _Copy"),
    0x5FE142: ("std::string::_Copy(n)", "allocates (n | 15) + 2 with the reference byte, copies"),
    0x5FE231: ("std::string::_Split()", "a shared buffer (reference byte not 0/255) is copied with assign"),
    0x5FDDA2: ("std::logic_error::logic_error(const_string&)", "exception base, _Str copy-assigned, logic_error vtable 0x725BEC"),
    0x5FDDED: ("std::logic_error::what()", "_Str.c_str()"),
    0x5FDDFA: ("std::logic_error::_Doraise()", "throws a copy of *this"),
    0x5FDE17: ("std::logic_error::~logic_error", "_Str._Tidy(true), ~exception"),
    0x5FDE33: ("std::logic_error::scalar_deleting_dtor", "~logic_error, frees when bit 0"),
    0x5FDE61: ("std::logic_error::logic_error(const_logic_error&)", "copies the exception and _Str"),
    0x5FDEA2: ("std::length_error::_Doraise()", "throws a copy of *this (length_error vtable 0x725BCC)"),
    0x5FDEC6: ("std::length_error::scalar_deleting_dtor", "~logic_error, frees when bit 0"),
    0x5FDEE2: ("std::length_error::length_error(const_length_error&)", "logic_error copy, length_error vtable"),
    0x5FDD4E: ("std::deque::_Xlen", "throws length_error(\"deque<T> too long\")"),
    0x5FD906: ("std::deque::_Growmap(n)", "_Xlen when the map cannot grow by n, allocates a larger map, frees the old"),
    0x5FD336: ("std::deque::push_back(value)", "_Growmap(1) when full, a 0xFA0-byte block when missing, ++_Mysize, copy-constructs"),
    0x5FD2EE: ("std::deque::back()", "*(begin() + _Mysize - 1)"),
    0x5FD2CC: ("std::deque::operator[](n)", "*(begin() + n)"),
    0x5FD4C1: ("std::deque::const_iterator::operator*", "_Map[(off / 25) mod _Mapsize] + (off % 25) * 0xA0"),
    0x5FD30D: ("std::deque::pop_front()", "++_Myoff (wraps at _Mapsize * 25), --_Mysize, _Myoff = 0 when empty"),
    0x5FD8C4: ("std::deque::pop_back()", "--_Mysize, _Myoff = 0 when empty"),
    0x5FD4A8: ("std::deque::_Tidy()", "pop_back() until empty, then frees the blocks and the map"),
    0x5FD228: ("std::deque::operator=", "copies over, erases or inserts the difference"),
    0x5FD3FF: ("std::deque::erase(first,last)", "copies the shorter side over the hole, pops the freed elements"),
    0x5FE35A: ("std::copy<deque::iterator>", "element-wise assignment, forward"),
    0x5FE317: ("std::copy_backward<deque::iterator>", "element-wise assignment, backward"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", required=True)
    ap.add_argument("--symbols", default="symbols/functions.tsv")
    ap.add_argument("--table", default="generated/table.cpp")
    ap.add_argument("--objdump", default="llvm-objdump")
    ap.add_argument("--out", default="out/stl")
    a = ap.parse_args()
    img = Image(a.exe, a.symbols, a.table, a.objdump)
    trees, role, tree_of = classify_trees(img)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "trees.tsv"), "w") as o:
        o.write("nil\tnilrefs\tnode\tkey\tmembers\n")
        for t in trees:
            o.write(f"{t['nil']:08X}\t{t['nilrefs']:08X}\t{t['node']:#x}\t"
                    f"{t['key'] or '-'}\t{len(t['members'])}\n")
    names = {}
    tinfo = {t["nil"]: t for t in trees}
    for m in role:
        t = tinfo[tree_of[m]]
        names[m] = (f"std::_Tree@{t['nil']:X}::{role[m]}",
                    f"<xtree> _Tree member {role[m]}: {EVIDENCE[role[m]]}. "
                    f"Instantiation: _Nil {t['nil']:#x}, _Nilrefs {t['nilrefs']:#x}, "
                    f"node {t['node']:#x} bytes, key {t['key'] or 'not compared'}")
    names.update(HAND)
    names.update(classify_lists(img, names.keys()))
    names.update(classify_vectors(img, names.keys()))
    kinds = collections.Counter(re.sub(r"@[^:]*", "", n) if not n.startswith("std::_Tree@")
                                else "std::_Tree::" + n.split("::", 2)[2]
                                for n, _ in names.values())
    print(f"{len(trees)} map/set instantiations; {len(names)} functions named", file=sys.stderr)
    for k, n in sorted(kinds.items(), key=lambda kv: (kv[0].split("::")[1], -kv[1])):
        print(f"{n:5d} {k}", file=sys.stderr)
    with open(os.path.join(a.out, "names.tsv"), "w") as o:
        for m in sorted(names):
            n, why = names[m]
            o.write(f"{m:08X}\t{n}\t{why}. From tools/stl_names.py.\n")


if __name__ == "__main__":
    main()
