# SPDX-License-Identifier: GPL-3.0-or-later
# Pack one or more PNG files into a multi-resolution Windows .ico, storing each
# image as a PNG-compressed entry (supported by the Win10/11 resource compiler
# and icon loader). Stdlib only -- no Pillow/ImageMagick required.
#
#   python make_ico.py out.ico 16.png 24.png 32.png 48.png 64.png 128.png 256.png
import struct
import sys


def png_size(data: bytes) -> tuple[int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    # IHDR width/height are big-endian uint32 at offsets 16 and 20.
    width = struct.unpack(">I", data[16:20])[0]
    height = struct.unpack(">I", data[20:24])[0]
    return width, height


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: make_ico.py out.ico in1.png [in2.png ...]", file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    images = []
    for path in sys.argv[2:]:
        with open(path, "rb") as fh:
            data = fh.read()
        w, h = png_size(data)
        images.append((w, h, data))
    images.sort(key=lambda t: t[0])  # ascending by width (cosmetic)

    count = len(images)
    out = bytearray()
    out += struct.pack("<HHH", 0, 1, count)  # ICONDIR: reserved, type=1 (icon), count
    offset = 6 + 16 * count
    for w, h, data in images:
        out += struct.pack(
            "<BBBBHHII",
            0 if w >= 256 else w,   # bWidth  (0 means 256)
            0 if h >= 256 else h,   # bHeight (0 means 256)
            0,                      # bColorCount (0 for >=256 colors)
            0,                      # bReserved
            1,                      # wPlanes
            32,                     # wBitCount
            len(data),              # dwBytesInRes
            offset,                 # dwImageOffset
        )
        offset += len(data)
    for _, _, data in images:
        out += data

    with open(out_path, "wb") as fh:
        fh.write(out)
    print(f"wrote {out_path}: {count} images ({', '.join(str(w) for w, _, _ in images)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
