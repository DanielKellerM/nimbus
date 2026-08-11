#!/usr/bin/env python3
# Post-link L2-SPM layout guard (chimera-sdk check_section_overlaps.py analog).
#
# gwaihir packs the host and the cluster/QCS structures into ONE L2-SPM aperture and
# carves it up by hand (firmware @0, descriptor @0x10000, arena, host data @0x40000,
# return-code page @top). Non-overlap is otherwise an unchecked agreement between
# independently linked binaries -- the l2spm-overlap failure class. This asserts the
# allocated L2 footprints of the given ELF(s) plus fixed reserved regions are pairwise
# disjoint and inside the aperture, and fails the build otherwise.
#
#   check_l2_overlap.py <elf>... --aperture BASE:SIZE [--reserve BASE:SIZE:NAME]... [--readelf readelf]
import argparse, re, subprocess, sys

_SEC = re.compile(r"\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S+\s+(\S+)")


def alloc_sections(elf, lo, hi, readelf):
    out = subprocess.run([readelf, "-SW", elf], capture_output=True, text=True, check=True).stdout
    secs = []
    for line in out.splitlines():
        m = _SEC.search(line)
        if not m:
            continue
        name, addr, size, flags = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        if "A" not in flags or size == 0 or not (lo <= addr < hi):
            continue
        secs.append((addr, addr + size, f"{elf.split('/')[-1]}:{name}"))
    return secs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elfs", nargs="+")
    ap.add_argument("--aperture", required=True, help="BASE:SIZE")
    ap.add_argument("--reserve", action="append", default=[], help="BASE:SIZE:NAME")
    ap.add_argument("--readelf", default="readelf")
    a = ap.parse_args()

    b, s = (int(x, 0) for x in a.aperture.split(":"))
    lo, hi = b, b + s
    ivals, bad = [], False

    for r in a.reserve:
        p = r.split(":")
        rb, rs = int(p[0], 0), int(p[1], 0)
        ivals.append((rb, rb + rs, p[2] if len(p) > 2 else "reserved"))
    for elf in a.elfs:
        for st, en, nm in alloc_sections(elf, lo, hi, a.readelf):
            if st < lo or en > hi:
                print(f"  OUT-OF-APERTURE {nm} [{st:#x},{en:#x}) not in [{lo:#x},{hi:#x})")
                bad = True
            ivals.append((st, en, nm))

    ivals.sort()
    for (s1, e1, n1), (s2, e2, n2) in zip(ivals, ivals[1:]):
        if e1 > s2:
            print(f"  OVERLAP {n1} [{s1:#x},{e1:#x})  vs  {n2} [{s2:#x},{e2:#x})")
            bad = True

    print(f"  L2-SPM aperture [{lo:#010x},{hi:#010x}):")
    for st, en, nm in ivals:
        print(f"    [{st:#010x},{en:#010x})  {nm}")
    print("L2-SPM layout: FAIL (overlap)" if bad else "L2-SPM layout: OK")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
