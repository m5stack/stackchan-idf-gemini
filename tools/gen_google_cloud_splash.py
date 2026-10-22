#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""Generate per-board Google Cloud boot-splash PNGs from Wikimedia artwork.

Source (CC BY 4.0):
  https://commons.wikimedia.org/wiki/File:Google_Cloud_logo.svg
  https://upload.wikimedia.org/wikipedia/commons/5/51/Google_Cloud_logo.svg

Renders a white canvas at each board's native resolution with the logo
centred. StopWatch (466x466 round) keeps the artwork inside the inscribed
circle so the square panel corners don't clip it.
"""
from __future__ import annotations

import io
import pathlib
import sys
import urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "assets" / "splash"
WIKI = "https://commons.wikimedia.org/wiki/File:Google_Cloud_logo.svg"
SVG_URL = "https://upload.wikimedia.org/wikipedia/commons/5/51/Google_Cloud_logo.svg"
PNG_FALLBACK = (
    "https://upload.wikimedia.org/wikipedia/commons/thumb/5/51/"
    "Google_Cloud_logo.svg/1280px-Google_Cloud_logo.svg.png"
)

# Board canvases: (filename, width, height, max_logo_w, max_logo_h)
# StopWatch inscribed-square side is 466/sqrt(2) ≈ 329; keep a margin.
TARGETS = [
    ("splash_cores3.png", 320, 240, 280, 120),
    ("splash_atom.png", 128, 128, 112, 56),
    ("splash_stopwatch.png", 466, 466, 300, 140),
]


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "stackchan-idf-gemini-splash/1.0"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def load_logo():
    from PIL import Image

    # Prefer a rasterized PNG (no cairo required). Fall back to the SVG only
    # if a renderer is present.
    try:
        data = fetch(PNG_FALLBACK)
        im = Image.open(io.BytesIO(data)).convert("RGBA")
        print(f"fetched PNG fallback {im.size} from {PNG_FALLBACK}", file=sys.stderr)
        return im
    except Exception as e:
        print(f"PNG fallback failed: {e}", file=sys.stderr)

    svg = fetch(SVG_URL)
    print(f"fetched SVG {len(svg)} B from {SVG_URL}", file=sys.stderr)
    try:
        import cairosvg  # type: ignore

        png = cairosvg.svg2png(bytestring=svg, output_width=1280)
        return Image.open(io.BytesIO(png)).convert("RGBA")
    except Exception as e:
        print(f"cairosvg unavailable ({e}); cannot rasterize SVG", file=sys.stderr)
        raise SystemExit(1) from e


def fit(im, max_w: int, max_h: int):
    from PIL import Image

    w, h = im.size
    scale = min(max_w / w, max_h / h)
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    return im.resize((nw, nh), Image.Resampling.LANCZOS)


def main() -> int:
    from PIL import Image

    OUT.mkdir(parents=True, exist_ok=True)
    logo = load_logo()
    # Drop near-white background so compositing onto our white canvas is clean.
    # The Wikimedia PNG is already transparent around the wordmark.

    source_note = OUT / "SOURCE.txt"
    source_note.write_text(
        "Google Cloud logo (full color wordmark)\n"
        f"Page: {WIKI}\n"
        f"File: {SVG_URL}\n"
        f"Raster used: {PNG_FALLBACK}\n"
        "License: Creative Commons Attribution 4.0 International (CC BY 4.0)\n"
        "Customer (Google) approved use of this trademark artwork in firmware.\n",
        encoding="utf-8",
    )

    for name, cw, ch, max_w, max_h in TARGETS:
        canvas = Image.new("RGBA", (cw, ch), (255, 255, 255, 255))
        mark = fit(logo, max_w, max_h)
        x = (cw - mark.size[0]) // 2
        y = (ch - mark.size[1]) // 2
        canvas.alpha_composite(mark, (x, y))
        out = canvas.convert("RGB")
        path = OUT / name
        out.save(path, format="PNG", optimize=True)
        print(f"wrote {path} {out.size} {path.stat().st_size} B", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ImportError:
        print("Pillow is required: pip install pillow", file=sys.stderr)
        raise SystemExit(1)
