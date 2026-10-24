# Writes neots2_test.pfa, a tiny Type 1 font for the text layout tests: box glyphs for a few letters,
# with 'f' as tall and 'g' as deep as the game's ITC Benguiat Gothic (714 and -230 font units), so
# the game's line measures come out as they do for its own fonts.
#   python make_test_font.py
import os

GLYPHS = {
    # name: (advance, box) with box = (x0, y0, x1, y1) in font units, or None
    ".notdef": (250, None),
    "space": (250, None),
    "H": (600, (0, 0, 600, 700)),
    "f": (300, (0, 0, 300, 714)),
    "g": (500, (0, -230, 500, 500)),
}


def number(v):
    if -107 <= v <= 107:
        return bytes([v + 139])
    if 108 <= v <= 1131:
        return bytes([(v - 108) // 256 + 247, (v - 108) % 256])
    if -1131 <= v <= -108:
        return bytes([(-v - 108) // 256 + 251, (-v - 108) % 256])
    return bytes([255]) + v.to_bytes(4, "big", signed=True)


def charstring(advance, box):
    hsbw, rmoveto, rlineto, closepath, endchar = 13, 21, 5, 9, 14
    out = number(0) + number(advance) + bytes([hsbw])
    if box:
        x0, y0, x1, y1 = box
        out += number(x0) + number(y0) + bytes([rmoveto])
        for dx, dy in ((x1 - x0, 0), (0, y1 - y0), (x0 - x1, 0)):
            out += number(dx) + number(dy) + bytes([rlineto])
        out += bytes([closepath])
    return out + bytes([endchar])


def encrypt(data, key):
    out = bytearray()
    for b in data:
        c = b ^ (key >> 8)
        key = ((c + key) * 52845 + 22719) & 0xFFFF
        out.append(c)
    return bytes(out)


def main():
    private = bytearray(b"\0\0\0\0")
    private += (b"dup /Private 8 dict dup begin\n"
                b"/RD{string currentfile exch readstring pop}executeonly def\n"
                b"/ND{noaccess def}executeonly def\n"
                b"/NP{noaccess put}executeonly def\n"
                b"/BlueValues [] ND\n"
                b"/MinFeature{16 16} ND\n"
                b"/password 5839 def\n"
                b"/lenIV 4 def\n")
    private += b"2 index /CharStrings %d dict dup begin\n" % len(GLYPHS)
    for name, (advance, box) in GLYPHS.items():
        code = encrypt(b"\0\0\0\0" + charstring(advance, box), 4330)
        private += b"/%s %d RD " % (name.encode(), len(code)) + code + b" ND\n"
    private += (b"end\nend\nreadonly put\nnoaccess put\n"
                b"dup/FontName get exch definefont pop\nmark currentfile closefile\n")
    hexed = encrypt(bytes(private), 55665).hex()
    lines = [hexed[i:i + 64] for i in range(0, len(hexed), 64)]

    head = """%!PS-AdobeFont-1.0: NeoTS2-Test 001.000
%%Title: NeoTS2-Test
11 dict begin
/FontInfo 8 dict dup begin
/version (001.000) readonly def
/FullName (NeoTS2 Test Bold) readonly def
/FamilyName (NeoTS2 Test) readonly def
/Weight (Bold) readonly def
/ItalicAngle 0 def
/isFixedPitch false def
/UnderlinePosition -100 def
/UnderlineThickness 50 def
end readonly def
/FontName /NeoTS2-Test def
/Encoding StandardEncoding def
/PaintType 0 def
/FontType 1 def
/FontMatrix [0.001 0 0 0.001 0 0] readonly def
/FontBBox {0 -230 600 714} readonly def
currentdict end
currentfile eexec
"""
    tail = "\n".join(["0" * 64] * 8) + "\ncleartomark\n"
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "neots2_test.pfa")
    with open(path, "w", newline="\n") as f:
        f.write(head + "\n".join(lines) + "\n" + tail)


if __name__ == "__main__":
    main()
