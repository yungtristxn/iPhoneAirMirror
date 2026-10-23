"""Rebuild the selected iPhone Air Mirror icon (development only; Pillow required).

The SVG is the 256px master. Each ICO frame is drawn separately: 16-32px
frames use a slightly larger mark, pixel-aligned edges and stronger strokes.
The selected concept remains untouched in local/release-branding/icon-option-1.png.
"""

from argparse import ArgumentParser
from pathlib import Path
import hashlib
import io
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
TILE = (13, 14, 238, 236)
TILE_RADIUS = 55
BACK = (89.5, 70.5, 206.5, 155.5)
FRONT = (44.5, 97.5, 173.5, 182.5)
SCREEN_STROKE = 11
SCREEN_RADIUS = 18
CHARCOAL = ((41, 46, 54), (13, 17, 23))
EDGE = ((57, 64, 73), (26, 32, 40))
TEAL = ((127, 204, 206), (111, 189, 191))
IVORY = ((246, 243, 236), (238, 235, 229))


def color(rgb):
    return "#" + "".join(f"{channel:02x}" for channel in rgb)


def svg_master():
    def gradient(name, palette, top, bottom):
        return (
            f'    <linearGradient id="{name}" gradientUnits="userSpaceOnUse" '
            f'x1="0" y1="{top}" x2="0" y2="{bottom}">\n'
            f'      <stop stop-color="{color(palette[0])}"/>\n'
            f'      <stop offset="1" stop-color="{color(palette[1])}"/>\n'
            "    </linearGradient>"
        )

    def screen(box, paint):
        left, top, right, bottom = box
        half = SCREEN_STROKE / 2
        return (
            f'  <rect x="{left + half:g}" y="{top + half:g}" '
            f'width="{right - left - SCREEN_STROKE:g}" '
            f'height="{bottom - top - SCREEN_STROKE:g}" '
            f'rx="{SCREEN_RADIUS - half:g}" fill="url(#charcoal)" '
            f'stroke="url(#{paint})" stroke-width="{SCREEN_STROKE}"/>'
        )

    gradients = "\n".join(
        gradient(name, palette, top, bottom)
        for name, palette, top, bottom in (
            ("charcoal", CHARCOAL, TILE[1], TILE[3]),
            ("edge", EDGE, TILE[1], TILE[3]),
            ("teal", TEAL, BACK[1], BACK[3]),
            ("ivory", IVORY, FRONT[1], FRONT[3]),
        )
    )
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" '
        'role="img" aria-labelledby="title">\n'
        '  <title id="title">iPhone Air Mirror</title>\n'
        "  <defs>\n" + gradients + "\n  </defs>\n"
        '  <rect x="14" y="15" width="223" height="220" rx="54" '
        'fill="url(#charcoal)" stroke="url(#edge)" stroke-width="2"/>\n'
        + screen(BACK, "teal") + "\n"
        + screen(FRONT, "ivory") + "\n</svg>\n"
    )


def render(size):
    if size not in SIZES:
        raise ValueError("Use one of the native ICO frame sizes")
    samples = 8 if size <= 64 else 4
    pixels = size * samples
    scale = pixels / 256
    image = Image.new("RGBA", (pixels, pixels))

    def inset(box, amount):
        left, top, right, bottom = box
        return left + amount, top + amount, right - amount, bottom - amount

    def mask(box, radius):
        shape = Image.new("L", (pixels, pixels))
        # Half-open bounds match SVG geometry and keep opposite edges balanced.
        left, top, right, bottom = (round(value * scale) for value in box)
        ImageDraw.Draw(shape).rounded_rectangle(
            (left, top, right - 1, bottom - 1),
            radius=max(1, round(radius * scale)), fill=255,
        )
        return shape

    def gradient(palette, top, bottom):
        layer = Image.new("RGBA", (pixels, pixels))
        painter = ImageDraw.Draw(layer)
        for y in range(pixels):
            t = min(1, max(0, ((y + 0.5) / scale - top) / (bottom - top)))
            rgba = tuple(round(a + (b - a) * t) for a, b in zip(*palette)) + (255,)
            painter.line((0, y, pixels - 1, y), fill=rgba)
        return layer

    charcoal = gradient(CHARCOAL, TILE[1], TILE[3])
    if size > 32:
        image.paste(gradient(EDGE, TILE[1], TILE[3]), (0, 0), mask(TILE, TILE_RADIUS))
        image.paste(charcoal, (0, 0), mask(inset(TILE, 2), TILE_RADIUS - 2))
    else:
        # The subtle tile rim is subpixel at these sizes; omit it to avoid a halo.
        image.paste(charcoal, (0, 0), mask(TILE, TILE_RADIUS))

    for box, palette in ((BACK, TEAL), (FRONT, IVORY)):
        stroke = SCREEN_STROKE
        radius = SCREEN_RADIUS
        if size <= 32:
            expansion = 1.08 if size <= 24 else 1.04
            box = tuple(128 + (value - 128) * expansion for value in box)
            # Align outer edges to the native pixel grid, not the supersample grid.
            box = tuple(round(value * size / 256) * 256 / size for value in box)
            stroke = max(stroke, 256 / size)
            radius = max(radius * expansion, 1.4 * 256 / size)
        image.paste(gradient(palette, box[1], box[3]), (0, 0), mask(box, radius))
        # Opaque interior covers the rear outline where the screens overlap.
        image.paste(charcoal, (0, 0), mask(inset(box, stroke), max(1, radius - stroke)))
    return image.resize((size, size), Image.Resampling.LANCZOS)


def ico_bytes(images):
    payloads = []
    for image in images:
        stream = io.BytesIO()
        image.save(stream, format="PNG", compress_level=9)
        payloads.append(stream.getvalue())
    offset = 6 + 16 * len(SIZES)
    header = bytearray(struct.pack("<HHH", 0, 1, len(SIZES)))
    for size, payload in zip(SIZES, payloads):
        header.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(payload), offset))
        offset += len(payload)
    return header + b"".join(payloads)


def contact_sheet(images, destination):
    # Generated artwork only: native pixels on light and dark taskbar-like backgrounds.
    widths = [max(120, icon.width + 32) for icon in images]
    width, row_height, header = sum(widths), 316, 38
    sheet = Image.new("RGB", (width, row_height * 2), "#e9e9e7")
    draw = ImageDraw.Draw(sheet)
    draw.rectangle((0, row_height, width, row_height * 2), fill="#181b21")
    font = ImageFont.load_default(size=14)
    for row, ink in ((0, "#262a30"), (row_height, "#e5e8ed")):
        x = 0
        for cell, icon in zip(widths, images):
            draw.text((x + 10, row + 10), f"{icon.width} x {icon.height}", fill=ink, font=font)
            sheet.paste(icon, (x + (cell - icon.width) // 2, row + header + (256 - icon.height) // 2), icon)
            x += cell
    destination.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(destination, format="PNG", compress_level=9)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--contact-sheet", type=Path, help="Optional generated native-size preview")
    args = parser.parse_args()
    images = [render(size) for size in SIZES]
    assets = ROOT / "assets"
    (assets / "app-icon.svg").write_text(svg_master(), encoding="utf-8", newline="\n")
    images[-1].save(assets / "app-icon.png", format="PNG", compress_level=9)
    (assets / "app-icon.ico").write_bytes(ico_bytes(images))
    if args.contact_sheet:
        contact_sheet(images, args.contact_sheet.resolve())
    for name in ("app-icon.svg", "app-icon.png", "app-icon.ico"):
        data = (assets / name).read_bytes()
        print(f"{name}: {len(data)} bytes; SHA-256 {hashlib.sha256(data).hexdigest()}")
    print(f"Generated {len(SIZES)} individually rendered native icon sizes: {SIZES}")


if __name__ == "__main__":
    main()
