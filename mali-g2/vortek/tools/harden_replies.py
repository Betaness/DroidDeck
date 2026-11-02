#!/usr/bin/env python3
"""Bounds-check the client's reply deserialisation (DroidDeck G2, mali-g2/vortek).

The Vortek client unserialises every server reply with the generated vt_unserialize_* functions of
include/vortek_serializer.h, which trust the server completely: lengths, counts, item sizes and
the pNext type list come straight from the reply and are used as offsets, loop bounds, memcpy
sizes and allocation sizes, and a truncated reply is read past its end. A bad reply therefore
crashes the client (inside the application, under Wine an exception in a Unix call).

This script rewrites the vt_unserialize_* functions of the CLIENT copy of the header
(work/vortek/include/vortek_serializer.h; the server's copy in winlator-app is left alone, so the
server's hot request path is unchanged) so that every read goes through the helpers that
vortek-0014 adds to vortek.h:

  *(T*)(inputBuffer + off), inputBuffer[off++]   ->  VT_IN(T, inputBuffer + off)
      read only inside [memoryPool->inStart, memoryPool->inEnd) (the reply vt_recv read); outside
      it the value is 0 and memoryPool->inBad is set
  memcpy(dst, inputBuffer + off, n)               ->  vt_inCopy(memoryPool, dst, cap, src, n)
      source range checked; cap is sizeof(dst) for a fixed array field, inCap elements for an
      array the application passed to a command (pData, pProperties ...), unknown (trusted:
      allocated from the same count just before) otherwise; fixed char arrays are NUL-terminated
  vt_alloc(memoryPool, count * size)              ->  vt_inAlloc(memoryPool, count, size)
      count must be >= 0 and at most the reply's size (every element takes at least a byte of it),
      a failed allocation returns zeroed scratch memory and marks the reply bad
  for (int i = 0; i < n; i++)                      ->  bounded by vt_inCount (n <= reply size),
      stops once the reply is bad, and fixed destination arrays (val->arr[i]) are index-checked
  while (pNextType != -1)                          ->  also stops once the reply is bad
  end of each function                            ->  vt_inOk(memoryPool, inputBuffer, bufferOffset):
      the reply must hold everything the function stepped over, read or not
  arrays the application passed to a command      ->  writes only below memoryPool->inCap
  if (pNextIsNULL) val->pNext = chain              ->  dropped: the chain lives in the call's
      memory pool, which is freed when the call returns; it must never be linked into the
      application's structure (a server that sent extension structures nobody asked for left a
      dangling pNext in the application's struct)

Any read of inputBuffer the rules do not recognise fails the script, so a new upstream or patch
pattern cannot slip through unchecked. The caller (vulkan_calls.c) sets the bounds, checks
memoryPool->inBad after unserialising and fails the call (VK_ERROR_DEVICE_LOST, or
VK_ERROR_INITIALIZATION_FAILED for instance-level calls) instead of crashing.

  harden_replies.py <work/vortek/include/vortek_serializer.h>   (rewrites in place, idempotent)
"""
import re
import sys

MARK = "/* [mali-g2] reply reads bounds-checked by tools/harden_replies.py */"

FUNC_RE = re.compile(r"^static inline void (vt_unserialize_\w+)\((.*)\) \{$")
READ_RE = re.compile(r"\*\(([A-Za-z_][\w ]*?\**)\*\)\((inputBuffer(?: \+ [^()]+)?)\)")
INDEX_RE = re.compile(r"inputBuffer\[(\w+(?:\+\+)?)\]")
MEMCPY_RE = re.compile(r"memcpy\(([^,]+), (inputBuffer[^,]*), (.+)\);")
ALLOC_RE = re.compile(r"vt_alloc\(memoryPool, (.+)\);")
FOR_RE = re.compile(r"for \(int i = 0; i < ([\w>-]+); i\+\+\) \{")
NESTED_RE = re.compile(r"vt_unserialize_\w+\(.*inputBuffer \+ bufferOffset, memoryPool\);")


def fail(lineno, line, why):
    sys.exit(f"harden_replies.py: line {lineno}: {why}:\n    {line.strip()}")


def alloc_args(expr, lineno, line):
    expr = expr.strip()
    m = re.fullmatch(r"(\w+) \* (.+)", expr)
    if m:
        return f"{m.group(1)}, {m.group(2)}"
    if re.fullmatch(r"sizeof\([^()]+\)", expr):
        return f"1, {expr}"
    if re.fullmatch(r"\w+", expr):
        return f"{expr}, 1"
    fail(lineno, line, "unrecognised allocation size")


def params_of(sig):
    names = []
    for p in sig.split(","):
        m = re.search(r"(\w+)\s*(?:\[\w*\])?$", p.strip())
        if m:
            names.append(m.group(1))
    return names


def transform(text):
    if MARK in text:
        return text, 0
    lines = text.split("\n")
    out = []
    func = None          # name of the vt_unserialize_* function being rewritten
    params = []
    toplevel = False     # vt_unserialize_vk*: a command, its array parameters are the application's
    arr_param = False    # current arrValues points at an application array
    has_offset = False   # the function walks the reply with bufferOffset
    changed = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        lineno = i + 1
        if func is None:
            m = FUNC_RE.match(line)
            if m:
                func = m.group(1)
                params = params_of(m.group(2))
                toplevel = func.startswith("vt_unserialize_vk")
                arr_param = False
                has_offset = False
            out.append(line)
            i += 1
            continue
        if line == "}":
            # the reply must also hold what was only skipped (fields read into a NULL pointer)
            if has_offset:
                out.append("    vt_inOk(memoryPool, inputBuffer, (size_t)bufferOffset);")
                changed += 1
            func = None
            out.append(line)
            i += 1
            continue
        if line.strip() == "int bufferOffset = 0;":
            has_offset = True

        new = line
        s = line.strip()

        # where arrValues points
        m = re.match(r"\s*(?:const )?[\w ]+\*+ arrValues = (\w+);", line)
        if m:
            arr_param = m.group(1) in params
        elif re.match(r"\s*(?:const )?[\w ]+\*+ arrValues = vt_alloc\(", line):
            arr_param = False

        if s == "if (pNextIsNULL) val->pNext = invertVkStructuresChain(pNext);":
            indent = line[: len(line) - len(line.lstrip())]
            new = indent + "(void)pNext; /* [mali-g2] never link pool memory into the application's structure */"
            out.append(new)
            changed += 1
            i += 1
            continue

        if s == "while (pNextType != -1) {":
            new = line.replace("while (pNextType != -1) {", "while (pNextType != -1 && !memoryPool->inBad) {")

        m = FOR_RE.search(new)
        if m:
            bound = m.group(1)
            cond = f"i < vt_inCount(memoryPool, {bound}) && !memoryPool->inBad"
            # fixed destination arrays written in the loop body (val->physicalDevices[i])
            depth, j, arrays = 0, i, []
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                for a in re.findall(r"(val->\w+)\[i\]", lines[j]):
                    if a not in arrays:
                        arrays.append(a)
                if depth <= 0 and j > i:
                    break
                j += 1
            for a in arrays:
                cond += f" && vt_inIndex(memoryPool, i, VT_COUNTOF({a}))"
            new = new.replace(m.group(0), f"for (int i = 0; {cond}; i++) {{")

        if toplevel and arr_param and re.match(r"\s*if \(arrValues\) \{", new):
            new = new.replace("if (arrValues) {", "if (arrValues && vt_inSlot(memoryPool, i)) {")

        m = MEMCPY_RE.search(new)
        if m:
            dst, src, size = m.group(1), m.group(2), m.group(3)
            if toplevel and (dst in params or (dst == "arrValues" and arr_param)):
                mm = re.fullmatch(r"(\w+) \* (.+)", size)
                cap = f"(size_t)memoryPool->inCap * ({mm.group(2)})" if mm else "(size_t)memoryPool->inCap"
            elif re.fullmatch(r"val->\w+", dst):
                cap = f"VT_DST_CAP({dst})"
            elif dst in ("val", "arrValues") or re.fullmatch(r"arrValues\[i\]", dst):
                cap = "(size_t)-1"
            else:
                fail(lineno, line, "unrecognised memcpy destination")
            if re.fullmatch(r"val->\w+", dst):
                repl = f"vt_inCopy(memoryPool, {dst}, {cap}, {src}, {size}), VT_IN_TERMINATE({dst});"
            else:
                repl = f"vt_inCopy(memoryPool, {dst}, {cap}, {src}, {size});"
            new = new.replace(m.group(0), repl)
            # a count beyond its fixed array (memoryTypeCount > VK_MAX_MEMORY_TYPES ...) is a bad reply
            if re.match(r"\s*if \(val->\w+ <= \w+\) vt_inCopy", new):
                new += " else memoryPool->inBad = true;"

        m = ALLOC_RE.search(new)
        if m:
            new = new.replace(m.group(0), f"vt_inAlloc(memoryPool, {alloc_args(m.group(1), lineno, line)});")

        new = READ_RE.sub(lambda mm: f"VT_IN({mm.group(1)}, {mm.group(2)})", new)
        new = INDEX_RE.sub(lambda mm: f"VT_IN(char, inputBuffer + {mm.group(1)})", new)

        # every remaining use of inputBuffer must be a checked helper or a nested unserialise call
        rest = re.sub(r"VT_IN\([^,]+, inputBuffer[^)]*\)", "", new)
        rest = re.sub(r"vt_inCopy\(memoryPool, .*?inputBuffer[^,]*, [^;]*;", "", rest)
        if "inputBuffer" in rest and not NESTED_RE.search(rest):
            fail(lineno, line, "unchecked read of the reply")
        if "vt_alloc(" in rest:
            fail(lineno, line, "unchecked allocation")

        if new != line:
            changed += 1
        out.append(new)
        i += 1

    text = "\n".join(out)
    # the marker goes after the include guard so that the file stays a valid header
    text = text.replace("#define VORTEK_SERIALIZER_H\n", "#define VORTEK_SERIALIZER_H\n" + MARK + "\n", 1)
    if MARK not in text:
        sys.exit("harden_replies.py: no VORTEK_SERIALIZER_H include guard")
    return text, changed


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    with open(path) as f:
        text = f.read()
    text, changed = transform(text)
    with open(path, "w") as f:
        f.write(text)
    print(f"harden_replies.py: {path}: {changed} lines rewritten" if changed else f"harden_replies.py: {path}: already hardened")


if __name__ == "__main__":
    main()
