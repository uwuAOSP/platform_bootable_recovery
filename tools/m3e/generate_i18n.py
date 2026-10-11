# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Generate bilingual UI strings and the licensed Chinese UI glyph subset.
Run on a development host with Pillow and --font /path/to/NotoSansSC.ttf.
The pinned source font is not needed on the cloud build server or the device.
Writes m3e_cjk.h and m3e_strings.h to recovery_ui/include/recovery_ui/
(override with --out DIR). The BASIC layout engine is pinned so the output
does not depend on whether Pillow was built with libraqm.
Existing glyphs are retained from the output atlas (or the checked-in atlas
when --out is empty), so host FreeType updates do not restyle other pages.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parents[1] / 'recovery_ui/include/recovery_ui'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--font', type=Path, required=True)
parser.add_argument('--out', type=Path, default=OUT, help='output directory')
args = parser.parse_args()
metadata = json.loads((ROOT / 'fonts/NotoSansSC-SOURCE.json').read_text())
assert hashlib.sha256(args.font.read_bytes()).hexdigest() == metadata[0]['sha256']
strings = json.loads((ROOT / 'translations.json').read_text(encoding='utf-8'))
codes = sorted({ord(c) for s in list(strings) + list(strings.values()) for c in s if ord(c) > 126})
existing = args.out / 'm3e_cjk.h'
if not existing.exists():
    existing = OUT / 'm3e_cjk.h'
old = existing.read_text(encoding='utf-8') if existing.exists() else ''
old_codes = list(map(int, re.search(r'kCjkCodes\[\] = \{([^}]+)', old)[1].split(','))) if old else []
legacy_ascent = int((re.search(r'kLegacyCjkAscent = (\d+)', old) or re.search(r'kCjkAscent = (\d+)', old))[1]) if old else 0
legacy_descent = int((re.search(r'kLegacyCjkDescent = (\d+)', old) or re.search(r'kCjkDescent = (\d+)', old))[1]) if old else 0
parts = ['/*', ' * SPDX-FileCopyrightText: The uwuAOSP Project',
         ' * SPDX-License-Identifier: Apache-2.0', ' */',
         '// UI glyph subset from Noto Sans SC; SIL OFL 1.1. See m3e-cjk-OFL.txt.',
         '#pragma once', '#include "m3e_font.h"',
         'namespace recovery_m3e { namespace fontdata {',
         f'inline constexpr int kCjkCount = {len(codes)};',
         'inline constexpr uint32_t kCjkCodes[] = {' + ','.join(map(str, codes)) + '};']
top_extent, bottom_extent = 0, 0
for style, weight in [('Regular', 400), ('Bold', 700)]:
    # Keep checked-in glyphs pixel-identical across host FreeType versions.
    # Rasterize only added characters, retaining the untouched page baseline.
    previous = {}
    if old:
        records = [tuple(map(int, g.split(','))) for g in re.findall(r'\{([^{}]+)\}',
                   re.search(r'kCjk'+style+r'\[kCjkCount\] = \{(.*?)\n\};', old, re.S)[1])]
        data = list(map(int, filter(None, re.search(r'kCjk'+style+r'Data\[\] = \{(.*?)\n\};',
                    old, re.S)[1].replace('\n', '').split(','))))
        previous = dict(zip(old_codes, records))
    font = ImageFont.truetype(str(args.font), 96, layout_engine=ImageFont.Layout.BASIC)
    font.set_variation_by_axes([weight])
    blob, glyphs = [], []
    for code in codes:
        if code in previous:
            offset, length, w, h, left, top, advance = previous[code]
            glyphs.append((len(blob), length, w, h, left, top, advance))
            blob.extend(data[offset:offset+length])
            top_extent, bottom_extent = max(top_extent, -top), max(bottom_extent, top+h)
            continue
        ch = chr(code)
        left, top, right, bottom = font.getbbox(ch, anchor='ls')
        w, h = right-left, bottom-top
        top_extent, bottom_extent = max(top_extent, -top), max(bottom_extent, bottom)
        mask = Image.new('L', (max(1, w), max(1, h)))
        ImageDraw.Draw(mask).text((-left, -top), ch, font=font, fill=255, anchor='ls')
        raw = mask.tobytes() if w and h else b''
        offset = len(blob)
        i = 0
        while i < len(raw):
            j = i + 1
            while j < len(raw) and raw[j] == raw[i] and j-i < 255:
                j += 1
            blob.extend((j-i, raw[i]))
            i = j
        glyphs.append((offset, len(blob)-offset, w, h, left, top, round(font.getlength(ch)*64)))
    parts.append(f'inline constexpr uint8_t kCjk{style}Data[] = {{')
    parts += [','.join(map(str, blob[i:i+32]))+',' for i in range(0, len(blob), 32)]
    parts += ['};', f'inline constexpr Glyph kCjk{style}[kCjkCount] = {{']
    parts += ['{'+','.join(map(str, g))+'},' for g in glyphs]
    parts.append('};')
parts += [f'inline constexpr int kCjkAscent = {top_extent};',
          f'inline constexpr int kCjkDescent = {bottom_extent};',
          f'inline constexpr int kLegacyCjkAscent = {legacy_ascent or top_extent};',
          f'inline constexpr int kLegacyCjkDescent = {legacy_descent or bottom_extent};', '}}', '']
(args.out / 'm3e_cjk.h').write_bytes('\n'.join(parts).encode())
parts = ['/*', ' * SPDX-FileCopyrightText: The uwuAOSP Project',
             ' * SPDX-License-Identifier: Apache-2.0', ' */',
         '// Generated from translations.json.',
         '#pragma once', '#include <atomic>', '#include <string>', '#include <string_view>',
         'namespace recovery_m3e {',
         'enum class Language { English, Chinese };',
         'inline std::atomic<Language> ui_language{Language::English};',
         'inline Language GetLanguage() { return ui_language.load(std::memory_order_relaxed); }',
         'inline void SetLanguage(Language language) { ui_language.store(language, std::memory_order_relaxed); }',
         'inline bool SupportedLocale(const std::string& locale) { return locale == "en-US" || locale == "zh-CN"; }',
         'inline Language LanguageForLocale(const std::string& locale) { return locale.rfind("zh", 0) == 0 ? Language::Chinese : Language::English; }',
         'struct Translation { std::string_view key, chinese; };',
         'inline constexpr Translation kTranslations[] = {']
parts += ['  {'+json.dumps(k,ensure_ascii=False)+','+json.dumps(v,ensure_ascii=False)+'},' for k,v in strings.items()]
parts += ['};', '''inline std::string Tr(const std::string& text) {
  if(GetLanguage()!=Language::Chinese) return text;
  for(const auto& entry:kTranslations) if(text==entry.key) return std::string(entry.chinese);
  for(const auto& entry:kTranslations) {
    size_t token=entry.key.find("%d");
    if(token==std::string_view::npos) continue;
    std::string prefix(entry.key.substr(0,token)),suffix(entry.key.substr(token+2));
    if(text.size()<=prefix.size()+suffix.size() || text.compare(0,prefix.size(),prefix)!=0 ||
        text.compare(text.size()-suffix.size(),suffix.size(),suffix)!=0) continue;
    std::string number=text.substr(prefix.size(),text.size()-prefix.size()-suffix.size());
    if(number.find_first_not_of("0123456789")!=std::string::npos) continue;
    std::string translated(entry.chinese);
    size_t placeholder=translated.find("%d");
    if(placeholder!=std::string::npos) translated.replace(placeholder,2,number);
    return translated;
  }
  size_t start=text.find_first_not_of(" "),end=text.find_last_not_of(" ");
  if(start!=std::string::npos) {
    std::string trimmed=text.substr(start,end-start+1);
    for(const auto& entry:kTranslations) if(trimmed==entry.key) return std::string(entry.chinese);
  }
  return text;
}''', '}', '']
(args.out / 'm3e_strings.h').write_bytes('\n'.join(parts).encode())
print(f'Generated {len(strings)} translations and {len(codes)} CJK glyphs in two weights.')
