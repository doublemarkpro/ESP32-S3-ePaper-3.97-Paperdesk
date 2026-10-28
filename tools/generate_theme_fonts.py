"""Rebuild the two numeric theme font sets with Pillow (Python 3.10+).

Sources and OFL licenses live in assets/fonts/<family>/source.json and OFL.txt.
Use --download to fetch the exact source revisions when local TTFs are absent.
The binary format matches generate_fonts.py; every glyph retains all its ink.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import urllib.request

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT_ROOT = ROOT / 'assets/fonts'
OUT = ROOT / 'firmware/main'
CHARACTERS = ' 0123456789:.-–/%°C…+'
SPECS = (
    ('theme_a_clock', 'barlow-condensed', 'BarlowCondensed-Light.ttf', 208),
    ('theme_a_large', 'barlow-condensed', 'BarlowCondensed-Regular.ttf', 112),
    ('theme_a_value', 'barlow-condensed', 'BarlowCondensed-Regular.ttf', 64),
    ('theme_b_clock', 'dm-serif-display', 'DMSerifDisplay-Regular.ttf', 168),
    ('theme_b_large', 'dm-serif-display', 'DMSerifDisplay-Regular.ttf', 104),
    ('theme_b_value', 'dm-serif-display', 'DMSerifDisplay-Regular.ttf', 68),
)


def source_files(download=False):
    for family in dict.fromkeys(spec[1] for spec in SPECS):
        directory = FONT_ROOT / family
        manifest = json.loads((directory / 'source.json').read_text(encoding='utf-8'))
        for entry in manifest['files']:
            path = directory / entry['file']
            if not path.exists():
                if not download:
                    raise FileNotFoundError(f'{path.name} missing; rerun with --download')
                request = urllib.request.Request(entry['url'], headers={'User-Agent': 'Paperdesk-font-builder'})
                with urllib.request.urlopen(request, timeout=30) as response:
                    data = response.read(2 * 1024 * 1024 + 1)
                if len(data) > 2 * 1024 * 1024:
                    raise ValueError('Font source exceeds expected size')
                if hashlib.sha256(data).hexdigest() != entry['sha256']:
                    raise ValueError(f'Unexpected source checksum: {entry["file"]}')
                path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                raise ValueError(f'Local source differs from pinned revision: {path}')


def measure_glyphs(font, size):
    measured = []
    origin = size * 2
    for char in sorted(set(CHARACTERS), key=ord):
        canvas = Image.new('L', (size * 4, size * 4), 0)
        ImageDraw.Draw(canvas).text((origin, origin), char, font=font, fill=255, anchor='ls')
        ink = canvas.point(lambda value: 255 if value >= 128 else 0, mode='1')
        bounds = ink.getbbox()
        advance = max(1, math.ceil(font.getlength(char)))
        if bounds:
            assert 0 < bounds[0] < bounds[2] < size * 4
            assert 0 < bounds[1] < bounds[3] < size * 4
            left, top, right, bottom = (v - origin for v in bounds)
            shift = max(0, -left)
            advance = max(advance, right + shift)
            measured.append((char, advance, left + shift, top, bottom, ink.crop(bounds)))
        else:
            measured.append((char, advance, 0, 0, 0, None))
    return measured


def clock_metrics(measured):
    glyphs = {item[0]: item for item in measured}
    def bounds(text):
        x = 0
        left, top, right, bottom = 10000, 10000, -1, -1
        for char in text:
            _, advance, gx, gy, gb, bitmap = glyphs[char]
            if bitmap is not None:
                left, top = min(left, x + gx), min(top, gy)
                right, bottom = max(right, x + gx + bitmap.width), max(bottom, gb)
            x += advance
        return {'advance': x, 'ink_width': right - left, 'ink_height': bottom - top}
    clocks = [f'{hour:02}:{minute:02}' for hour in range(24) for minute in range(60)]
    maximum = max(clocks, key=lambda value: bounds(value)['advance'])
    assert bounds(maximum)['advance'] <= 408, (maximum, bounds(maximum))
    return {'11:48': bounds('11:48'), '23:59': bounds('23:59'), 'widest': {maximum: bounds(maximum)}}


def generate(name, family, filename, size):
    font = ImageFont.truetype(str(FONT_ROOT / family / filename), size)
    measured = measure_glyphs(font, size)
    top = min(item[3] for item in measured if item[5] is not None)
    bottom = max(item[4] for item in measured if item[5] is not None)
    baseline = 1 - top
    cell = max(size, bottom - top + 2, max(item[1] for item in measured))
    records, bitmaps = bytearray(), bytearray()
    ink_pixels = 0
    for char, advance, left, glyph_top, _, ink in measured:
        records += struct.pack('<IHH', ord(char), advance, baseline)
        canvas = Image.new('1', (cell, cell), 0)
        if ink is not None:
            y = baseline + glyph_top
            assert left >= 0 and y >= 1 and left + ink.width <= cell and y + ink.height < cell
            canvas.paste(ink, (left, y))
            expected = sum(byte.bit_count() for byte in ink.tobytes())
            actual = sum(byte.bit_count() for byte in canvas.tobytes())
            assert actual == expected, f'Clipped {char!r} in {name}'
            ink_pixels += actual
        bitmaps += canvas.tobytes()
    data = struct.pack('<HH', cell, len(measured)) + records + bitmaps
    (OUT / f'{name}.bin').write_bytes(data)
    report = {'font': name, 'design_size': size, 'cell': cell, 'baseline': baseline,
              'glyphs': len(measured), 'verified_ink_pixels': ink_pixels, 'bytes': len(data)}
    if name.endswith('_clock'):
        report['clock_metrics'] = clock_metrics(measured)
    print(json.dumps(report, ensure_ascii=True))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--download', action='store_true', help='Download missing pinned sources after verifying SHA256')
    options = parser.parse_args()
    source_files(options.download)
    for spec in SPECS:
        generate(*spec)
