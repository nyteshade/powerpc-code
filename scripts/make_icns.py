#!/usr/bin/env python3
"""Build a Mac OS X .icns from a square PNG.

There is no iconutil here -- it is 10.7 and later, and this is a Leopard target
built from Linux. ImageMagick has no ICNS writer either. The format is simple
enough to emit directly, and doing so means the icon is reproducible from its
source rather than being a binary nobody can regenerate.

  ./scripts/make_icns.py art/ppcode_9000.png resources/appicon.icns

Layout: 'icns', total length, then chunks of (4-byte type, length including the
8-byte header, data).

Leopard needs the classic RLE types for the small sizes and JPEG 2000 for the
large ones. See LARGE below for the part that is easy to get wrong.
"""
import struct
import subprocess
import sys

# Classic types: (icns type, mask type, edge). Each carries three RLE-compressed
# colour planes plus a separate uncompressed 8-bit mask.
CLASSIC = [
    (b"is32", b"s8mk", 16),
    (b"il32", b"l8mk", 32),
    (b"ih32", b"h8mk", 48),
    (b"it32", b"t8mk", 128),
]

# Large types, 10.5 and later. JPEG 2000, *not* PNG.
#
# This is the one non-obvious thing about targeting Leopard. ic08 and ic09 hold
# PNG on 10.6 and later, and every modern description of the format says so --
# but 10.5 expects JPEG 2000 in those slots, and a PNG there does not merely get
# skipped: ImageIO rejects the entire file, so the application falls back to a
# generic icon with nothing to explain why. Verified on the G5 by feeding both
# to sips, which decodes the JP2 build at 512 and refuses the PNG one outright.
LARGE = [(b"ic08", 256), (b"ic09", 512)]


def rgba(src, size):
    """Raw straight-alpha RGBA bytes for `src` scaled to size x size."""
    out = subprocess.run(
        ["convert", src, "-alpha", "on", "-resize", f"{size}x{size}!",
         "-depth", "8", "RGBA:-"],
        check=True, stdout=subprocess.PIPE).stdout
    if len(out) != size * size * 4:
        raise SystemExit(f"expected {size*size*4} bytes for {size}px, got {len(out)}")
    return out


def jp2(src, size):
    return subprocess.run(
        ["convert", src, "-alpha", "on", "-resize", f"{size}x{size}!",
         "-depth", "8", "JP2:-"],
        check=True, stdout=subprocess.PIPE).stdout


def pack_channel(data):
    """PackBits as icns uses it, over one colour plane.

    A control byte below 0x80 means (n + 1) literal bytes follow. A control byte
    of 0x80 or more means (n - 0x80 + 3) copies of the single byte that follows.
    Runs are therefore worth encoding from three bytes up, which is why the
    literal and run cases overlap at two.
    """
    out = bytearray()
    i, n = 0, len(data)

    while i < n:
        # How far the run of identical bytes at i extends, capped at 130.
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 130:
            run += 1

        if run >= 3:
            out.append(0x80 + run - 3)
            out.append(data[i])
            i += run
            continue

        # Otherwise gather literals until a run of three shows up, capped at 128.
        start = i
        while i < n and (i - start) < 128:
            if (i + 2 < n and data[i] == data[i + 1] == data[i + 2]):
                break
            i += 1

        out.append(i - start - 1)
        out.extend(data[start:i])

    return bytes(out)


def chunk(kind, payload):
    return kind + struct.pack(">I", len(payload) + 8) + payload


def build(src, dest):
    chunks = []

    for kind, mask_kind, size in CLASSIC:
        px = rgba(src, size)
        r = px[0::4]
        g = px[1::4]
        b = px[2::4]
        a = px[3::4]

        body = pack_channel(r) + pack_channel(g) + pack_channel(b)
        # it32 alone carries four zero bytes before the compressed planes.
        if kind == b"it32":
            body = b"\x00\x00\x00\x00" + body

        chunks.append(chunk(kind, body))
        chunks.append(chunk(mask_kind, a))

    for kind, size in LARGE:
        chunks.append(chunk(kind, jp2(src, size)))

    body = b"".join(chunks)
    with open(dest, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(body) + 8) + body)

    print(f"wrote {dest}: {len(body) + 8} bytes, {len(chunks)} chunks")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_icns.py <source.png> <out.icns>")
    build(sys.argv[1], sys.argv[2])
