#!/usr/bin/env python3
"""Check the small arithmetic probe's actual code, allowing only 060 forms."""
from pathlib import Path
import re
import sys
nm = Path(sys.argv[1]).read_text()
dis = Path(sys.argv[2]).read_text()
assert 'file format' in dis, 'No object disassembly'
assert not re.search(r'\b_+(?:muldi3|divdi3|udivdi3)\b', nm), nm
for line in dis.splitlines():
    parts = line.split('\t')
    if len(parts) < 3 or not re.match(r'(?:muls|mulu|divs|divu)', parts[-1].strip()):
        continue
    words = parts[1].split()
    if len(words) < 2:
        continue
    try:
        opcode, extension = int(words[0], 16), int(words[1], 16)
    except ValueError:
        continue
    assert not (opcode & 0xff80 == 0x4c00 and extension & 0x400), line
print('AAC 060 probe: no extended-result multiply/divide or 64-bit helper calls')
