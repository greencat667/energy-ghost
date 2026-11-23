#!/usr/bin/env python3
"""
One-time art generator for Energy Ghost's mascot: Draw Things (local Stable
Diffusion app, Automatic1111-compatible API on port 7860) generates each
mood's ghost illustration, then this thresholds it to pure black/white,
resizes to ART_SIZE x ART_SIZE, packs it MSB-first (device's drawBitmap()
format), and writes energy_ghost/ghost_art.h.

Six moods, one drawing each - VERY_LOW through VERY_HIGH read the grid's
carbon intensity from a hollow, sparkly, smiling ghost through to a solid
black, jagged, alarmed one; UNKNOWN is the "no reading yet" cold-start
state. Same character across all six (consistent style prompt, no fixed
seed needed - the shared STYLE string alone is
enough to keep a consistent look).

Needs Pillow (pip install pillow) and Draw Things' API server enabled. Run
from the tools/ folder:
    python3 gen_ghost_art.py
"""
import base64
import io
import json
import urllib.request

from PIL import Image

DRAW_THINGS_URL = "http://127.0.0.1:7860"
GEN_SIZE = 512
# Kept smaller than a "fill the canvas" size deliberately - the longest
# recommendation strings (region-suffixed, e.g. "AVOID IF POSSIBLE - 412
# gCO2/kWh, no relief in sight (South West England)") wrap to 3 lines at
# CANVAS_SIZE-16 width, and this leaves enough room below the art for that
# without running off the 200px panel - see render_ghost()'s layout.
ART_SIZE = 104
STEPS = 8

STYLE = (
    "a cute simple ghost mascot character floating, thick bold black ink "
    "outline, flat line drawing, no color, no grayscale, no gradient, "
    "sticker illustration style, centered, plenty of white margin around it"
)
NEGATIVE = (
    "color, gradient, photorealistic, text, watermark, blurry, signature, "
    "multiple ghosts, extra limbs, 3d render, realistic, background scenery"
)

# (C identifier, prompt subject) - one per GhostMood enum value, same order
# as ghost_render.h so GHOST_ART[] can be indexed directly by the enum.
MOODS = [
    ("ART_VERY_LOW", "a happy content ghost, closed peaceful crescent-shaped "
        "eyes, gentle smile, small sparkle stars floating around it, hollow "
        "outline only with no fill, light and airy"),
    ("ART_LOW", "a cheerful smiling ghost, round open eyes, relaxed smile, "
        "hollow outline only with no fill"),
    ("ART_MODERATE", "a neutral calm ghost, plain round eyes, flat "
        "straight mouth, faint light dot speckle texture across its body"),
    ("ART_HIGH", "a worried ghost, furrowed angled eyebrows, a frown, one "
        "sweat drop, dense diagonal hatching texture filling its whole body"),
    ("ART_VERY_HIGH", "a scary alarmed ghost, wide panicked eyes, a sharp "
        "jagged zigzag mouth, a ragged torn jagged bottom edge instead of a "
        "smooth one, its whole body filled solid black"),
    ("ART_UNKNOWN", "a confused ghost, question-mark shaped eyes, a small "
        "dashed outline, simple and mostly empty, hollow with no fill"),
]


def generate(prompt):
    payload = {
        "prompt": f"{STYLE}, {prompt}",
        "negative_prompt": NEGATIVE,
        "width": GEN_SIZE,
        "height": GEN_SIZE,
        "steps": STEPS,
    }
    req = urllib.request.Request(
        f"{DRAW_THINGS_URL}/sdapi/v1/txt2img",
        data=json.dumps(payload).encode("utf-8"),
        headers={"content-type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    png_bytes = base64.b64decode(result["images"][0])
    return Image.open(io.BytesIO(png_bytes)).convert("L")


def to_1bpp_bytes(img):
    img = img.resize((ART_SIZE, ART_SIZE), Image.LANCZOS)
    bw = img.point(lambda p: 255 if p >= 140 else 0)
    px = bw.load()
    row_bytes = (ART_SIZE + 7) // 8
    out = bytearray(row_bytes * ART_SIZE)
    for y in range(ART_SIZE):
        for bx in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = bx * 8 + bit
                if x >= ART_SIZE:
                    break
                # Device convention bit=1
                # -> drawn/foreground (black ink), matching Adafruit_GFX's
                # drawBitmap() where a set bit paints.
                if px[x, y] < 128:  # black ink pixel
                    byte |= 0x80 >> bit
            out[y * row_bytes + bx] = byte
    return bytes(out)


def emit_header(art_bytes, path):
    row_bytes = (ART_SIZE + 7) // 8
    total_bytes = row_bytes * ART_SIZE
    lines = [
        "#ifndef GHOST_ART_H",
        "#define GHOST_ART_H",
        "",
        "#include <pgmspace.h>",
        "",
        f"#define ART_SIZE {ART_SIZE}  // {ART_SIZE}x{ART_SIZE}, {total_bytes} bytes each",
        "",
    ]
    for name, data in art_bytes:
        lines.append(f"static const uint8_t {name}[{total_bytes}] PROGMEM = {{")
        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
        lines.append("};")
        lines.append("")
    # Indexed by GhostMood enum order (ghost_render.h) so render_ghost() can
    # just do GHOST_ART[mood].
    lines.append("static const uint8_t *const GHOST_ART[] = {")
    for name, _ in art_bytes:
        lines.append(f"  {name},")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    art_bytes = []
    for name, prompt in MOODS:
        print(f"generating {name} ({prompt[:60]}...)...")
        img = generate(prompt)
        data = to_1bpp_bytes(img)
        art_bytes.append((name, data))
    out_path = "../energy_ghost/ghost_art.h"
    emit_header(art_bytes, out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
