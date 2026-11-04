#!/usr/bin/env python3
"""Exercise the native loader against independently generated BMP/ZIP files."""
import io
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile
import zlib
import sys

ROOT = Path(__file__).resolve().parents[1]
RUNNER = Path(os.environ.get("SKIN_TEST_RUNNER", str(ROOT / "skin_format_test")))


def bmp(w, h, bits=24, *, compressed=False, topdown=False):
    palette = b"".join(bytes((i*17, i*17, i*17, 0)) for i in range(16))
    if bits == 8:
        palette = b"".join(bytes((i, i, i, 0)) for i in range(256))
    elif bits == 1:
        palette = b"\0\0\0\0\xff\xff\xff\0"
    elif bits > 8:
        palette = b""
    rows = []
    expected = []
    for y in range(h):
        pixels = [((x+y) % (1 << bits) if bits <= 8 else (x*7+y*3) % 256) for x in range(w)]
        expected.append(b"".join(bytes((v*17 if bits == 4 else v*255 if bits == 1 else v,))*3 for v in pixels))
        if bits in (24, 32):
            row = b"".join(bytes((v, v, v)) + (b"\xff" if bits == 32 else b"") for v in pixels)
        elif bits == 8:
            row = bytes(pixels)
        elif bits == 4:
            row = bytes((pixels[x] << 4) | (pixels[x+1] if x+1<w else 0) for x in range(0,w,2))
        else:
            row = bytes(sum(pixels[x+i] << (7-i) for i in range(min(8,w-x))) for x in range(0,w,8))
        rows.append(row + b"\0" * (-len(row) % 4))
    if compressed:
        assert bits in (4, 8)
        out = bytearray()
        for y in reversed(range(h)):
            # One-pixel encoded runs exercise palette order and row reversal.
            for x in range(w):
                v = (x+y) % (1 << bits)
                out += bytes((1, v if bits == 8 else v << 4))
            out += b"\0\0"
        out += b"\0\1"
        pixels = bytes(out)
    else:
        pixels = b"".join(rows if topdown else reversed(rows))
    offset = 54 + len(palette)
    header = b"BM" + struct.pack("<IHHI", offset+len(pixels), 0, 0, offset)
    header += struct.pack("<IiiHHIIiiII", 40, w, -h if topdown else h, 1, bits,
                          (1 if bits == 8 else 2) if compressed else 0,
                          len(pixels), 0, 0, len(palette)//4, 0)
    return header + palette + pixels, b"".join(expected)


def archive(assets, compression=zipfile.ZIP_DEFLATED):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression) as z:
        for name, data in assets:
            z.writestr(name, data)
    return out.getvalue()


def run_tests(extra_skin=None):
    count = 0
    with tempfile.TemporaryDirectory(prefix="mintamp-skins-") as tmp:
        tmp = Path(tmp)

        def invoke(data, *, is_bmp=False, good=True, colours=False):
            nonlocal count
            path = tmp / ("sample.bmp" if is_bmp else "sample.wsz")
            path.write_bytes(data)
            cmd = [str(RUNNER)] + (["--bmp"] if is_bmp else ["--colours"] if colours else []) + [str(path)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            assert proc.returncode == (0 if good else 1), (cmd, proc.returncode, proc.stdout, proc.stderr)
            count += 1
            return proc.stdout

        for bits in (1, 4, 8, 24, 32):
            for topdown in (False, True):
                data, pixels = bmp(13, 7, bits, topdown=topdown)
                assert invoke(data, is_bmp=True).strip() == f"13 7 {zlib.crc32(pixels):08x}"
        for bits in (4, 8):
            data, pixels = bmp(13, 7, bits, compressed=True)
            assert invoke(data, is_bmp=True).strip() == f"13 7 {zlib.crc32(pixels):08x}"
            invoke(data[:-1], is_bmp=True, good=False)
        # Absolute runs, their padding, and delta skips (bottom-up).
        rle = b"\x00\x05\x01\x02\x03\x04\x05\0\0\0\0\x02\x01\0\x03\x07\0\x01"
        data, _ = bmp(5, 2, 8, compressed=True)
        offset = struct.unpack_from("<I", data, 10)[0]
        actual = data[:offset] + rle
        expected = bytes((0,0,0,7,7,7,7,7,7,7,7,7,0,0,0,1,1,1,2,2,2,3,3,3,4,4,4,5,5,5))
        assert invoke(actual, is_bmp=True).strip() == f"5 2 {zlib.crc32(expected):08x}"
        # Malformed headers and RLE row overflow must fail cleanly.
        for at, value in ((18,0xffffffff), (22,0x7fffffff), (10,0xffffffff), (30,99)):
            broken = bytearray(data); struct.pack_into("<I", broken, at, value)
            invoke(broken, is_bmp=True, good=False)
        invoke(data[:offset] + b"\x06\x01\0\x01", is_bmp=True, good=False)
        for n in (0, 1, 13, 53, offset-1):
            invoke(data[:n], is_bmp=True, good=False)

        assets=[]
        for name, w, h in (("MAIN.BMP",275,116), ("cbuttons.bmp",136,36), ("Text.BmP",155,12)):
            image, _ = bmp(w,h,8,compressed=True)
            assets.append(("Example/"+name,image))
        for compression in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED):
            invoke(archive(assets,compression))
        invoke(archive(assets[:-1]), good=False)
        invoke(archive(assets+[('MAIN.BMP',assets[0][1])]),good=False)
        invoke(archive([('MAIN.BMP',bmp(20,20)[0])]+assets[1:]),good=False)
        eq = ('Theme/EQMAIN.BMP', bmp(275,315,8)[0])
        invoke(archive(assets+[eq]))
        eqex=('Theme/EQ_EX.BMP',bmp(275,56)[0])
        invoke(archive(assets+[eq,eqex]))
        invoke(archive(assets+[eqex,eqex]),good=False)
        invoke(archive(assets+[('EQ_EX.BMP',bmp(274,29)[0])]),good=False)
        colors=b'\r\n'.join(f'{i},{i+1},{i+2} ; comment'.encode() for i in range(24))
        invoke(archive(assets+[('Theme/VISCOLOR.TXT',colors)]))
        palette_path=tmp/'palette.wsz'
        palette_path.write_bytes(archive(assets+[('Theme/VISCOLOR.TXT',colors)]))
        result=subprocess.run([str(RUNNER),'--visual-colours',str(palette_path)],capture_output=True,text=True,check=True)
        assert result.stdout.split()==[f'{(i<<16)|((i+1)<<8)|(i+2):06x}' for i in range(24)]
        palette_path.write_bytes(archive(assets+[('VISCOLOR.TXT',b'999,0,0')]))
        result=subprocess.run([str(RUNNER),'--visual-colours',str(palette_path)],capture_output=True,text=True,check=True)
        assert result.stdout.split()[0]=='000000' and result.stdout.split()[23]=='ffffff'
        invoke(archive(assets+[('viscolor.txt',b'invalid'),('VISCOLOR.TXT',colors)]),good=False)
        invoke(archive(assets+[('VISCOLOR.TXT',b'x'*8193)]),good=False)
        invoke(archive(assets+[('EQMAIN.BMP',bmp(275,314)[0])]),good=False)
        invoke(archive(assets+[eq,eq]),good=False)
        playlist = ('Theme/PLEDIT.BMP', bmp(280,186,8)[0])
        theme = ('Theme/PLEDIT.TXT', b'[Text]\r\nNormal=#102030\r\nCurrent=#aBcDeF\r\nNormalBG=#223344\r\nSelectedBG=#556677\r\nMbBG=#010203\r\nMbFG=#A0B0C0\r\nFont=Arial\r\n')
        invoke(archive(assets+[playlist]))
        invoke(archive(assets+[playlist,theme]))
        assert invoke(archive(assets+[theme]),colours=True).strip() == '102030 abcdef 223344 556677 010203 a0b0c0'
        assert invoke(archive(assets),colours=True).strip() == '00ff00 ffffff 000000 0000c6 000000 00d000'
        invalid = ('pledit.txt', b'[Other]\nNormal=#112233\n[Text]\nNormal=#zzzzzz\nCurrent=#12345\n')
        assert invoke(archive(assets+[invalid]),colours=True).strip() == '00ff00 ffffff 000000 0000c6 000000 00d000'
        spaced = ('pledit.txt', b' [TEXT] \r\n Normal = #AbCdEf \r\n Current=#001122\n[Other]\nCurrent=#ffffff\n')
        assert invoke(archive(assets+[spaced]),colours=True).strip() == 'abcdef 001122 000000 0000c6 000000 00d000'
        invoke(archive(assets+[playlist,playlist]),good=False)
        invoke(archive(assets+[theme,theme]),good=False)
        invoke(archive(assets+[('pledit.txt',b'x'*8193)]),good=False)
        invoke(archive(assets+[('pledit.bmp',bmp(275,109)[0])]),good=False)
        # Optional MB.BMP (artwork frame): loaded when big enough; an
        # undersized sheet is dropped without failing the skin.
        mb=('Theme/MB.BMP',bmp(242,119,8)[0])
        out=invoke(archive(assets+[mb]))
        assert any(l.split()[:3]==['14','242','119'] for l in out.splitlines()),out
        out=invoke(archive(assets+[('mb.bmp',bmp(177,119)[0])]))
        assert not any(l.split()[0]=='14' for l in out.splitlines()),out
        invoke(archive(assets+[mb,mb]),good=False)
        packed=archive(assets)
        for n in (0, 1, 21, len(packed)-1, len(packed)//2): invoke(packed[:n],good=False)
        broken=bytearray(packed)
        at=broken.index(b'PK\x01\x02'); broken[at+16]^=1
        invoke(broken,good=False) # CRC mismatch
        broken=bytearray(packed); struct.pack_into('<I',broken,at+24,0xffffffff)
        invoke(broken,good=False) # inflated length cap
        broken=bytearray(packed); struct.pack_into('<I',broken,at+42,0xffffffff)
        invoke(broken,good=False) # local-header offset
        if extra_skin:
            proc=subprocess.run([str(RUNNER),str(extra_skin)],capture_output=True,text=True)
            assert proc.returncode==0,proc.stderr
            # Pillow is an independent decoder for the user's reference skin.
            from PIL import Image
            names=('main','cbuttons','titlebar','numbers','text','volume','balance','posbar','playpaus','monoster','shufrep','pledit','eqmain','eq_ex','mb')
            reported={int(line.split()[0]): line.split()[1:] for line in proc.stdout.splitlines()}
            with zipfile.ZipFile(extra_skin) as z:
                for name in z.namelist():
                    base=Path(name).stem.lower()
                    if base not in names or not name.lower().endswith('.bmp'): continue
                    data=z.read(name)
                    image=Image.open(io.BytesIO(data))
                    try:
                        pixels=image.convert('RGB').tobytes()
                    except ValueError:
                        # Pillow rejects some valid early-EOB/delta RLE BMPs.
                        # ImageMagick decodes them (and can warn about padding).
                        decoded=subprocess.run(['convert','bmp:-','rgb:-'],input=data,capture_output=True)
                        pixels=decoded.stdout
                        assert len(pixels)==image.width*image.height*3,decoded.stderr
                    assert reported[names.index(base)]==[str(image.width),str(image.height),f'{zlib.crc32(pixels):08x}'],name
            count+=1
    print(f"Classic skin tests passed ({count} cases)")


if __name__ == '__main__':
    run_tests(Path(sys.argv[1]) if len(sys.argv)>1 else None)
