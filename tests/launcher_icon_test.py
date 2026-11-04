from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mintamp_launcher_icon import generate, ROOT
b = (ROOT / 'MintAMP-Launch.info').read_bytes()
original = (ROOT / 'MintAMP.info').read_bytes()
assert b == generate()
assert b[:54] == original[:54] and b[58:74] == original[58:74]
pos = 78
for _ in range(2):
    _, _, w, h, depth = struct.unpack_from('>hhhhh', b, pos)
    pos += 20 + ((w + 15) // 16) * 2 * h * depth
assert b[78:pos] == original[78:pos]  # exact existing icon artwork
count = struct.unpack_from('>I', b, pos)[0] // 4 - 1
pos += 4
values = []
for _ in range(count):
    n = struct.unpack_from('>I', b, pos)[0]; pos += 4
    assert b[pos+n-1] == 0
    values.append(b[pos:pos+n-1].decode()); pos += n
assert values == ['(GUI=REACTION)', '(MODE=CLASSIC)', '(SKIN=PROGDIR:Skins/MintAMP-mintleaf.wsz)', '(DOUBLESIZE=NO)']
assert b[pos:] == original[8310:] and b[pos:pos+4] == b'FORM'
print('Workbench launcher icon: planar/colour artwork preserved, ToolTypes verified')
