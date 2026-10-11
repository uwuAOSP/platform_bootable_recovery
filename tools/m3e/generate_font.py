# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Generate the small, licensed ASCII Google Sans atlas used by native Recovery.
Requires Pillow on the development host only. No font libraries run on device.
Writes recovery_ui/include/recovery_ui/m3e_font.h (override with --out DIR).
The BASIC layout engine is pinned so the output does not depend on whether
Pillow was built with libraqm (raqm yields fractional advances).
"""
import argparse
from pathlib import Path
import hashlib
import json
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parents[1] / 'recovery_ui/include/recovery_ui'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=OUT, help='output directory')
args = parser.parse_args()
SIZE = 96
source = ROOT / 'fonts/GoogleSans.ttf'
metadata = json.loads((ROOT / 'fonts/SOURCE.json').read_text())
assert hashlib.sha256(source.read_bytes()).hexdigest() == metadata[0]['sha256']
parts = ['// Generated from Google Sans, SIL OFL 1.1. See m3e-font-OFL.txt.',
         '#pragma once', '#include <cstdint>', 'namespace recovery_m3e { namespace fontdata {',
         'struct Glyph { uint32_t offset, length; uint16_t w, h; int16_t left, top; uint16_t advance; };',
         'inline constexpr int kSize = 96;']
for name, weight in [('Regular', 400), ('Bold', 700)]:
    font = ImageFont.truetype(str(source), SIZE, layout_engine=ImageFont.Layout.BASIC)
    axes = font.get_variation_axes()
    font.set_variation_by_axes([weight if b'weight' in a['name'].lower() else a['default'] for a in axes])
    ascent, descent = font.getmetrics()
    if name == 'Regular':
        parts += [f'inline constexpr int kAscent = {ascent};', f'inline constexpr int kDescent = {descent};']
    blob, glyphs = [], []
    for code in range(32, 127):
        ch = chr(code)
        left, top, right, bottom = font.getbbox(ch, anchor='ls')
        w, h = right-left, bottom-top
        mask = Image.new('L', (max(1,w), max(1,h)))
        if w and h:
            ImageDraw.Draw(mask).text((-left,-top),ch,font=font,fill=255,anchor='ls')
        raw = mask.tobytes() if w and h else b''
        offset = len(blob)
        i = 0
        while i < len(raw):
            j = i + 1
            while j < len(raw) and raw[j] == raw[i] and j-i < 255:
                j += 1
            blob.extend((j-i, raw[i])); i = j
        glyphs.append((offset,len(blob)-offset,w,h,left,top,round(font.getlength(ch)*64)))
    parts.append(f'inline constexpr uint8_t k{name}Data[] = {{')
    parts += [','.join(map(str,blob[i:i+32]))+',' for i in range(0,len(blob),32)]
    parts += ['};', f'inline constexpr Glyph k{name}[95] = {{']
    parts += ['{'+','.join(map(str,g))+'},' for g in glyphs]
    parts.append('};')
parts += ['}}  // namespace recovery_m3e::fontdata', '']
target=args.out/'m3e_font.h'
target.write_bytes('\n'.join(parts).encode())
print(f'{target.name}: {target.stat().st_size} bytes; font licensed under SIL OFL 1.1')
