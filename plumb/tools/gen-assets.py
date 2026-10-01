#!/usr/bin/env python3
# Copyright (c) 2026 The Plumb developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regenerate the Plumb badge and the PNG surfaces.

Reads plumb/filters.json, checks it against PLUMB_FILTERS in src/init.cpp,
writes the filter badge, and renders the README header, social preview,
X banner, avatars and favicons with rsvg-convert. Rendering needs the
Cinzel, IBM Plex Sans and IBM Plex Mono fonts installed. Fontconfig cannot
pick weight 600 out of the variable Cinzel font, so make a static instance
named "Cinzel SemiBold" (fonttools varLib.instancer, wght=600) first.

Run from the repository root: plumb/tools/gen-assets.py
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "plumb" / "assets"
PNG = ASSETS / "png"

RED = "#C8102E"
WHITE = "#FFFFFF"
PARCHMENT = "#F4EFE6"
CHARCOAL = "#1B1B1B"
SLATE = "#5C5C5C"

# Mark geometry from the brand handoff, viewBox -72 -72 144 144.
ARMS = (
    '<path d="M-8,-10 Q-10,-38 -22,-52 L22,-52 Q10,-38 8,-10 Z"/>'
    '<path d="M10,-8 Q38,-10 52,-22 L52,22 Q38,10 10,8 Z"/>'
    '<path d="M-10,-8 Q-38,-10 -52,-22 L-52,22 Q-38,10 -10,8 Z"/>'
    '<circle cx="0" cy="0" r="15"/>'
    '<path d="M-6,10 L6,10 L6,15 C6,20 18,20 18,28 L18,36 L0,72 L-18,36 L-18,28 C-18,20 -6,20 -6,15 Z"/>'
)
BAIL = '<rect x="-4" y="-57" width="8" height="7"/><circle cx="0" cy="-63" r="9"/>'


def mark(mask_id, fill, x, y, height):
    """The full mark with the ring, placed with its top left at x,y."""
    scale = height / 144
    return (
        f'<defs><mask id="{mask_id}" maskUnits="userSpaceOnUse" x="-72" y="-72" width="144" height="144">'
        '<rect x="-72" y="-72" width="144" height="144" fill="#fff"/>'
        '<circle cx="0" cy="0" r="6.5" fill="#000"/><circle cx="0" cy="-63" r="4.5" fill="#000"/>'
        '</mask></defs>'
        f'<g transform="translate({x + 52 * scale:.2f},{y + 72 * scale:.2f}) scale({scale:.4f})">'
        f'<g fill="{fill}" mask="url(#{mask_id})">{ARMS}{BAIL}</g></g>'
    )


def lockup(mask_id, fill, x, center_y, font_size):
    """Horizontal lockup: mark at 1.4x cap height, gap 0.45x mark width, wordmark centered."""
    cap = 0.70 * font_size
    height = 1.4 * cap
    width = height * 104 / 144
    text_x = x + width + 0.45 * width
    return (
        mark(mask_id, fill, x, center_y - height / 2, height)
        + f'<text x="{text_x:.1f}" y="{center_y + cap / 2:.1f}" fill="{fill}" font-family="Cinzel SemiBold, Cinzel" '
        f'font-weight="600" font-size="{font_size}" letter-spacing="{0.26 * font_size:.1f}">PLUMB</text>'
    )


def svg(width, height, body):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">{body}</svg>\n'
    )


def header(width, height, pad):
    return svg(width, height,
        f'<rect width="{width}" height="{height}" fill="{CHARCOAL}"/>'
        + lockup("h", WHITE, pad, height / 2, 104)
        + f'<text x="{width - pad}" y="{height / 2 + 12}" fill="{PARCHMENT}" font-family="IBM Plex Sans" '
        f'font-size="34" text-anchor="end">Every filter. Every release.</text>'
        f'<rect x="{width - pad - 430}" y="{height / 2 + 36}" width="430" height="1" fill="{SLATE}"/>')


def social():
    return svg(1280, 640,
        f'<rect width="1280" height="640" fill="{PARCHMENT}"/>'
        + lockup("s", RED, 96, 210, 112)
        + f'<text x="96" y="390" fill="{CHARCOAL}" font-family="IBM Plex Sans" font-weight="500" '
        f'font-size="52">Pure bitcoin. No spam tolerated.</text>'
        f'<rect x="96" y="460" width="1088" height="84" fill="{CHARCOAL}"/>'
        f'<text x="128" y="512" fill="{PARCHMENT}" font-family="IBM Plex Mono" font-size="30">'
        '$ git clone https://github.com/plumb-node/plumb</text>')


def badge(count):
    label = f"filters {count}"
    right = 14 + 7 * len(label)
    width = 24 + right
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="20" role="img" aria-label="{label}">'
        f'<title>{label}</title><rect width="24" height="20" fill="{CHARCOAL}"/>'
        f'<rect x="24" width="{right}" height="20" fill="{RED}"/>'
        '<svg x="4" y="2" width="16" height="16" viewBox="-66 -56 132 132"><defs><mask id="m">'
        '<rect x="-90" y="-100" width="180" height="200" fill="#fff"/><circle cx="0" cy="0" r="6.5" fill="#000"/>'
        f'</mask></defs><g fill="{WHITE}" mask="url(#m)">{ARMS}</g></svg>'
        f'<text x="{24 + right / 2:.0f}" y="14" fill="{WHITE}" font-family="Verdana,DejaVu Sans,sans-serif" '
        f'font-size="11" text-anchor="middle">{label}</text></svg>\n'
    )


def check_init(filters):
    init = (ROOT / "src" / "init.cpp").read_text()
    block = re.search(r"PLUMB_FILTERS\{\{(.*?)\}\};", init, re.DOTALL)
    if not block:
        sys.exit("PLUMB_FILTERS not found in src/init.cpp")
    in_init = re.findall(r'\{"(-\w+)", \w+, "(knots#\d+)"\}', block.group(1))
    in_json = [(f["option"], f["source"]) for f in filters]
    if in_init != in_json:
        sys.exit(f"src/init.cpp PLUMB_FILTERS {in_init} does not match plumb/filters.json {in_json}")


def render(src, dest, width, height=None):
    cmd = ["rsvg-convert", "-w", str(width)]
    if height:
        cmd += ["-h", str(height)]
    subprocess.run(cmd + ["-o", str(dest), str(src)], check=True)


def main():
    filters = json.loads((ROOT / "plumb" / "filters.json").read_text())
    check_init(filters)
    (ASSETS / "badge-filters.svg").write_text(badge(len(filters)))

    PNG.mkdir(exist_ok=True)
    sources = {
        "readme-header": header(1280, 320, 80),
        "social-preview": social(),
        "x-banner": header(1500, 500, 140),
    }
    for name, text in sources.items():
        path = ASSETS / f"{name}.svg"
        path.write_text(text)
        render(path, PNG / f"{name}.png", int(re.search(r'width="(\d+)"', text).group(1)))
    for size in (16, 32, 48):
        render(ASSETS / "plumb-favicon.svg", PNG / f"favicon-{size}.png", size, size)
    for size in (180, 400, 512):
        render(ASSETS / "plumb-avatar-512.svg", PNG / f"avatar-{size}.png", size, size)


if __name__ == "__main__":
    main()
