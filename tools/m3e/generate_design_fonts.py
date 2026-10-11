# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Generate the SVG-page font atlases; requires Pillow only on the host.

Sources and checksums are pinned in fonts/DESIGN-SOURCE.json. Device builds
consume this generated header and do not load TTF files or font libraries.
Material Symbols uses the checked-in subset, preserving the original glyphs.
"""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, default=ROOT.parents[1] / 'recovery_ui/include/recovery_ui')
args = parser.parse_args()
metadata = json.loads((ROOT / 'fonts/DESIGN-SOURCE.json').read_text())
for entry in metadata['fonts']:
    assert hashlib.sha256((ROOT / 'fonts' / entry['file']).read_bytes()).hexdigest() == entry['sha256']
SIZE = 96
parts = ['/*', ' * SPDX-FileCopyrightText: The uwuAOSP Project',
         ' * SPDX-License-Identifier: Apache-2.0', ' */',
         '// Generated glyph data: Google Sans Flex, Google Sans Code and Outfit (SIL OFL 1.1);',
         '// Material Symbols Rounded (Apache-2.0). See the corresponding m3e-*-OFL.txt and',
         '// m3e-symbols-LICENSE.txt files and tools/m3e/fonts/DESIGN-SOURCE.json.',
         '#pragma once', '#include "m3e_font.h"', 'namespace recovery_m3e::fontdata {']
icons = metadata['symbols']
parts.append('enum class Symbol { ' + ', '.join(icons) + ' };')
for family, filename in [('Flex', 'GoogleSansFlex.ttf'), ('Outfit', 'Outfit.ttf'),
                         ('Code', 'GoogleSansCode.ttf'), ('Symbols', 'MaterialSymbolsRounded.ttf')]:
    codes = list(icons.values()) if family == 'Symbols' else list(range(32, 127))
    for style, weight in [('Regular', 400), ('Bold', 700)]:
        font = ImageFont.truetype(str(ROOT / 'fonts' / filename), SIZE, layout_engine=ImageFont.Layout.BASIC)
        axes = font.get_variation_axes()
        values = []
        for axis in axes:
            name = axis['name'].lower()
            # Symbol fill is independent of weight; 400 matches the SVG icons.
            value = (400 if family == 'Symbols' else weight) if b'weight' in name else 24 if b'optical' in name else (style == 'Bold') if b'fill' in name else axis['default']
            values.append(max(axis['minimum'], min(axis['maximum'], value)))
        font.set_variation_by_axes(values)
        ascent, descent = font.getmetrics()
        if style == 'Regular':
            parts += [f'inline constexpr int k{family}Ascent = {ascent};',
                      f'inline constexpr int k{family}Descent = {descent};']
        blob, glyphs = [], []
        for code in codes:
            ch = chr(code)
            left, top, right, bottom = font.getbbox(ch, anchor='ls')
            w, h = right - left, bottom - top
            mask = Image.new('L', (max(1, w), max(1, h)))
            if w and h:
                ImageDraw.Draw(mask).text((-left, -top), ch, font=font, fill=255, anchor='ls')
            raw = mask.tobytes() if w and h else b''
            offset, i = len(blob), 0
            while i < len(raw):
                j = i + 1
                while j < len(raw) and raw[j] == raw[i] and j - i < 255:
                    j += 1
                blob.extend((j - i, raw[i])); i = j
            glyphs.append((offset, len(blob) - offset, w, h, left, top, round(font.getlength(ch) * 64)))
        parts.append(f'inline constexpr uint8_t k{family}{style}Data[] = {{')
        parts += [','.join(map(str, blob[i:i+32])) + ',' for i in range(0, len(blob), 32)]
        parts += ['};', f'inline constexpr Glyph k{family}{style}[{len(codes)}] = {{']
        parts += ['{' + ','.join(map(str, g)) + '},' for g in glyphs]
        parts.append('};')
parts += ['} // namespace recovery_m3e::fontdata', '']
target = args.out / 'm3e_design_fonts.h'
target.write_bytes('\n'.join(parts).encode())
print(f'{target}: {target.stat().st_size} bytes')
