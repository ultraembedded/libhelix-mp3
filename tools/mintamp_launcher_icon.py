#!/usr/bin/env python3
"""Reuse MintAMP's Workbench artwork; add optional, disabled ToolTypes.

Preserve both classic planar images and the existing OS3.5 FORM ICON extension.
Defaults are disabled so next-launch preferences can take effect.
"""
import struct
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def generate():
    data = bytearray((ROOT / 'MintAMP.info').read_bytes())
    assert struct.unpack_from('>HH', data) == (0xe310, 1)
    assert data[48] == 3 and struct.unpack_from('>I', data, 54)[0] == 0
    end = 78
    for _ in range(2):
        _, _, width, height, depth = struct.unpack_from('>hhhhh', data, end)
        assert 0 < depth <= 8
        end += 20 + ((width + 15) // 16) * 2 * height * depth
    assert data[end:end+4] == b'FORM'
    struct.pack_into('>I', data, 54, 1)
    struct.pack_into('>I', data, 74, 32768)
    types = ['(GUI=REACTION)', '(MODE=CLASSIC)',
             '(SKIN=PROGDIR:Skins/MintAMP-mintleaf.wsz)', '(DOUBLESIZE=NO)']
    strings = [text.encode('ascii') + b'\0' for text in types]
    table = struct.pack('>I', (len(strings) + 1) * 4)
    table += b''.join(struct.pack('>I', len(text)) + text for text in strings)
    return bytes(data[:end]) + table + bytes(data[end:])

if __name__ == '__main__':
    (ROOT / 'MintAMP-Launch.info').write_bytes(generate())
