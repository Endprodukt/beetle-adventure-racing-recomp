#!/usr/bin/env python3
"""Locate generated game functions accessing BAR's controller-layout globals.

Run from the repository root after generating RecompiledFuncs:
    py scripts/find-layout-references.py > layout-code.txt
The generated files stay local; this prints only small source excerpts.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent / "RecompiledFuncs"
ADDRESSES = ("8002cd40", "8002d064", "8002d01c")
# MIPS LUI/offset pairs split an address into two instructions. For an address
# with bit 15 set in its low half, the LUI high half is adjusted to 0x8003.
# N64Recomp therefore need not emit the full 0x8002.... value on any one line.
OFFSETS = re.compile(r"\b0[xX](?:FFFF)?(?:CD40|D064|D01C|32C0|2F9C|2FE4)\b", re.IGNORECASE)
FUNCTION = re.compile(r"\bRECOMP_FUNC\s+void\s+(\w+)\s*\(")


def main() -> None:
    files = sorted(ROOT.glob("*.c"))
    if not files:
        print(f"No generated .c files found under {ROOT}")
        return

    hits = 0
    offset_hits = 0
    for path in files:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        current_function = "(unknown)"
        for index, line in enumerate(lines):
            match = FUNCTION.search(line)
            if match:
                current_function = match.group(1)
            full = any(address in line.lower() for address in ADDRESSES)
            offset = OFFSETS.search(line) is not None
            if not full and not offset:
                continue
            hits += 1
            if offset and not full:
                offset_hits += 1
            print(f"\n{path.relative_to(ROOT.parent)}:{index + 1} {current_function}"
                  f" ({'full address' if full else 'address part; verify base'})")
            for context in range(max(0, index - 5), min(len(lines), index + 6)):
                print(f"  {context + 1:>6}: {lines[context]}")
    print(f"\nFound {hits} candidate references ({offset_hits} partial) in {len(files)} generated files.")


if __name__ == "__main__":
    main()
