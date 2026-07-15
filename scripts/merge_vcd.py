#!/usr/bin/env python3
"""Merge several VCD files onto a common time axis into one VCD.

Usage: merge_vcd.py out.vcd in1.vcd in2.vcd [...]

Each input keeps its own $scope tree (so a verilated model's TOP and a
veroutines tb sit side by side). Codes are remapped to stay unique, and
timescales are normalised to the finest input unit.
"""
import re
import sys

UNIT_FS = {"s": 10**15, "ms": 10**12, "us": 10**9, "ns": 10**6, "ps": 10**3, "fs": 1}


def code_gen():
    i = 0
    while True:
        n, s = i, ""
        while True:
            s += chr(33 + n % 94)
            n //= 94
            if n == 0:
                break
        yield s
        i += 1


def parse_timescale(text):
    m = re.match(r"\s*(\d+)\s*([a-z]+)", text)
    return int(m.group(1)) * UNIT_FS[m.group(2)]


def fmt_timescale(fs):
    for mult in (1, 10, 100):
        for unit, uf in UNIT_FS.items():
            if fs == mult * uf:
                return f"{mult}{unit}"
    return f"{fs}fs"


def parse_vcd(path):
    lines = open(path).read().splitlines()
    ts_fs, decls, events, codes = 1, [], [], set()

    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("$timescale"):
            body = line[len("$timescale"):]
            while "$end" not in body:
                i += 1
                body += " " + lines[i].strip()
            ts_fs = parse_timescale(body.split("$end")[0])
        elif line.startswith("$scope"):
            p = line.split()
            decls.append(("scope", p[2] if len(p) > 2 else "m"))
        elif line.startswith("$upscope"):
            decls.append(("upscope",))
        elif line.startswith("$var"):
            p = line.split()
            width, code, name = p[2], p[3], p[4]
            extra = " ".join(q for q in p[5:] if q != "$end")
            decls.append(("var", width, code, name + ((" " + extra) if extra else "")))
            codes.add(code)
        elif line.startswith("$enddefinitions"):
            i += 1
            break
        i += 1

    t = 0
    for line in lines[i:]:
        line = line.strip()
        if not line or line.startswith("$"):
            continue
        if line[0] == "#":
            t = int(line[1:])
        elif line[0] in "bBrR":
            v, c = line.split()[0], line.split()[1]
            events.append((t, c, v))
        else:
            events.append((t, line[1:], line[0]))
    return ts_fs, decls, events, codes


def main():
    out, ins = sys.argv[1], sys.argv[2:]
    parsed = [parse_vcd(p) for p in ins]
    common = min(p[0] for p in parsed)

    gen = code_gen()
    all_decls, merged = [], []
    for ts_fs, decls, events, codes in parsed:
        rm = {c: next(gen) for c in codes}
        for d in decls:
            all_decls.append(("var", d[1], rm[d[2]], d[3]) if d[0] == "var" else d)
        scale = ts_fs // common
        merged.extend((t * scale, rm[c], v) for (t, c, v) in events)

    merged.sort(key=lambda e: e[0])

    def emit(f, code, val):
        f.write(f"{val} {code}\n" if val[0] in "bBrR" else f"{val}{code}\n")

    with open(out, "w") as f:
        f.write(f"$timescale {fmt_timescale(common)} $end\n")
        for d in all_decls:
            if d[0] == "scope":
                f.write(f"$scope module {d[1]} $end\n")
            elif d[0] == "upscope":
                f.write("$upscope $end\n")
            else:
                f.write(f"$var wire {d[1]} {d[2]} {d[3]} $end\n")
        f.write("$enddefinitions $end\n#0\n$dumpvars\n")

        cur = 0
        for (t, c, v) in merged:
            if t == 0:
                emit(f, c, v)
        f.write("$end\n")
        for (t, c, v) in merged:
            if t == 0:
                continue
            if t != cur:
                f.write(f"#{t}\n")
                cur = t
            emit(f, c, v)


if __name__ == "__main__":
    main()
