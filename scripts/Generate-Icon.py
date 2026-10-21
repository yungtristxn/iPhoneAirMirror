"""Rebuild the original app icon at native Windows sizes (development only).
Geometry and palette match assets/app-icon.svg. Requires Pillow, not at runtime.
"""
from pathlib import Path
import io
import struct
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def render(size):
    scale = size * 4 / 256
    n = size * 4
    mask = Image.new("L", (n, n))
    d = ImageDraw.Draw(mask)
    box = lambda coords: tuple(round(v * scale) for v in coords)
    d.rounded_rectangle(box((16, 16, 240, 240)), radius=round(52 * scale), fill=255)
    gradient = Image.new("RGBA", (n, n))
    start, end = (33, 143, 234), (38, 61, 164)
    for y in range(n):
        t = y / max(1, n - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(start, end)) + (255,)
        ImageDraw.Draw(gradient).line((0, y, n, y), fill=color)
    image = Image.new("RGBA", (n, n))
    image.paste(gradient, (0, 0), mask)
    d = ImageDraw.Draw(image)
    d.rounded_rectangle(box((120, 72, 208, 188)), radius=round(16*scale),
                        fill="#204b91", outline="#a4d8ff", width=max(1, round(9*scale)))
    d.rounded_rectangle(box((66, 46, 148, 210)), radius=round(20*scale),
                        fill="#152e58", outline="#f7fbff", width=max(1, round(11*scale)))
    if size >= 24:
        d.rounded_rectangle(box((95, 188.5, 119, 193.5)), radius=max(1, round(2.5*scale)), fill="#b3d6ff")
    return image.resize((size, size), Image.Resampling.LANCZOS)


images = [render(size) for size in SIZES]
payloads = []
for image in images:
    stream = io.BytesIO()
    image.save(stream, format="PNG")
    payloads.append(stream.getvalue())
offset = 6 + 16 * len(SIZES)
header = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
for size, payload in zip(SIZES, payloads):
    header.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(payload), offset))
    offset += len(payload)
(ROOT / "assets/app-icon.ico").write_bytes(header + b"".join(payloads))
images[-1].save(ROOT / "assets/app-icon.png")
print(f"Generated {len(SIZES)} native icon sizes; {offset} bytes")
