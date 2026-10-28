"""Generate reproducible 1-bit bitmap fonts from OFL-licensed Noto Sans SC.

Binary: u16 cell_size,u16 count; count*(u32 codepoint,u16 advance,u16 baseline);
then count square row-major bitmaps, MSB first. The filename is the design font
size; cell_size includes the full measured ascender/descender range. Keeping a
common baseline avoids clipping Chinese strokes and Latin descenders without
shrinking the requested font. No runtime font engine or SD card is required.
"""
from pathlib import Path
import struct, math
from PIL import Image, ImageFont, ImageDraw

ROOT=Path(__file__).resolve().parents[1]
FONT=ROOT/'assets/fonts/NotoSansSC.ttf'
OUT=ROOT/'firmware/main'

def generate(size,characters):
    font=ImageFont.truetype(str(FONT),size)
    if hasattr(font,'set_variation_by_name'):
        font.set_variation_by_name('Bold' if size>=56 else 'Medium' if size>=24 else 'Regular')
    chars=sorted(set(characters),key=ord)
    # Measure rasterized ink on an oversized baseline-anchored canvas. Font
    # getbbox() includes advance bearings and does not exactly match the final
    # 1-bit threshold, so use the pixels that the firmware will actually draw.
    measured=[]; origin=size*2
    for char in chars:
        canvas=Image.new('L',(size*4,size*4),0)
        ImageDraw.Draw(canvas).text((origin,origin),char,font=font,fill=255,anchor='ls')
        ink=canvas.point(lambda value:255 if value>=128 else 0,mode='1')
        bounds=ink.getbbox()
        advance=max(1,math.ceil(font.getlength(char)))
        if bounds:
            assert 0<bounds[0]<bounds[2]<size*4 and 0<bounds[1]<bounds[3]<size*4
            left,top,right,bottom=(value-origin for value in bounds)
            # Negative left bearing occurs for j and a few accented glyphs.
            # Translate only those glyphs; normal CJK spacing remains unchanged.
            shift=max(0,-left)
            advance=max(advance,right+shift)
            measured.append((char,advance,left+shift,top,bottom,ink.crop(bounds)))
        else:
            measured.append((char,advance,0,0,0,None))
    top=min(item[3] for item in measured if item[5] is not None)
    bottom=max(item[4] for item in measured if item[5] is not None)
    baseline=1-top
    cell=max(size,bottom-top+2,max(item[1] for item in measured))
    records=bytearray();bitmaps=bytearray();ink_pixels=0
    for char,advance,left,glyph_top,_,ink in measured:
        records+=struct.pack('<IHH',ord(char),advance,baseline)
        im=Image.new('1',(cell,cell),0)
        if ink is not None:
            y=baseline+glyph_top
            assert left>=0 and y>=1 and left+ink.width<=cell and y+ink.height<cell
            im.paste(ink,(left,y))
            expected=sum(byte.bit_count() for byte in ink.tobytes())
            actual=sum(byte.bit_count() for byte in im.tobytes())
            assert actual==expected, f'Clipped glyph U+{ord(char):04X} at size {size}'
            ink_pixels+=actual
        bitmaps+=im.tobytes()
    path=OUT/f'font{size}.bin'
    path.write_bytes(struct.pack('<HH',cell,len(chars))+records+bitmaps)
    print(f'{path.name}: {len(chars)} glyphs, cell={cell}, baseline={baseline}, '
          f'ink_y=1..{baseline+bottom-1}, {ink_pixels} verified ink pixels, '
          f'{path.stat().st_size} bytes')

if __name__=='__main__':
    ascii_chars=''.join(chr(c) for c in range(32,127))+'°℃·—→…％：，。／'
    chinese=''
    for high in range(0xa1,0xf8):
        for low in range(0xa1,0xff):
            try:chinese+=bytes([high,low]).decode('gb2312')
            except UnicodeDecodeError:pass
    source=''.join(p.read_text(encoding='utf-8') for p in OUT.glob('*.cpp'))
    subset=ascii_chars+''.join(c for c in source if ord(c)>127)
    for size in (20,24,32):generate(size,ascii_chars+chinese+subset)
    for size in (56,112):generate(size,ascii_chars)
    generate(144,' 0123456789:-')
