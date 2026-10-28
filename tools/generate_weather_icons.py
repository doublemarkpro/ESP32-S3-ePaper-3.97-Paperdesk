"""Generate Paperdesk's original 1-bit weather line-art and visual QA atlas.

Geometry is rendered directly at each native size; no third-party SVG paths
are embedded. The legacy 64 px assets remain unchanged for the home screen.
The 96/112/128 px variants use fine 2.2 px strokes instead of enlarging pixels.
Code meanings follow QWeather's official condition table:
https://dev.qweather.com/en/docs/api/weather/weather-conditions/
https://icons.qweather.com/en/icons/ (night variants)

Pillow is a build-time dependency only. The firmware needs no graphics library.
"""
from pathlib import Path
import math
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[1]
S=4

class Pen:
    def __init__(self,size=64):
        self.scale=S*size/64
        self.stroke_ratio=1 if size==64 else 64/size*(2.2/2.6)
        self.im=Image.new('L',(size*S,size*S),0)
        self.d=ImageDraw.Draw(self.im)
    def line(self,points,width=2.6,ink=255):
        points=[(round(x*self.scale),round(y*self.scale)) for x,y in points]
        stroke=width*self.stroke_ratio*self.scale
        self.d.line(points,fill=ink,width=round(stroke),joint='curve')
        r=stroke/2
        for x,y in (points[0],points[-1]):self.d.ellipse((x-r,y-r,x+r,y+r),fill=ink)
    def circle(self,x,y,r,width=2.6,fill=None):
        self.d.ellipse(tuple(round(v*self.scale) for v in (x-r,y-r,x+r,y+r)),fill=fill,outline=255,width=round(width*self.stroke_ratio*self.scale))
    def path(self,start,curves,width=2.6,fill=None):
        points=[start];p=start
        for c1,c2,end in curves:
            for i in range(1,33):
                t=i/32;u=1-t
                points.append((u*u*u*p[0]+3*u*u*t*c1[0]+3*u*t*t*c2[0]+t*t*t*end[0],u*u*u*p[1]+3*u*u*t*c1[1]+3*u*t*t*c2[1]+t*t*t*end[1]))
            p=end
        if fill is not None:self.d.polygon([(round(x*self.scale),round(y*self.scale)) for x,y in points],fill=fill)
        self.line(points,width)

def sun(p,x=32,y=32,r=13,rays=26):
    p.circle(x,y,r)
    for n in range(8):
        a=n*math.pi/4
        p.line([(x+math.cos(a)*(rays-6),y+math.sin(a)*(rays-6)),(x+math.cos(a)*rays,y+math.sin(a)*rays)])

def moon(p,x=0,y=0,scale=1):
    tr=lambda v:(x+v[0]*scale,y+v[1]*scale)
    p.path(tr((43,9)),[tuple(tr(v) for v in c) for c in [((28,5),(13,16),(13,31)),((13,48),(29,58),(44,51)),((31,49),(24,40),(25,29)),((25,20),(31,12),(43,9))]],width=2.6,fill=0)

def cloud(p,y=0,small=False):
    def tr(v):
        x,yy=v
        return (9+x*.78,yy*.78+y+9) if small else (x,yy+y)
    p.path(tr((17,46)),[tuple(tr(v) for v in c) for c in [((7,46),(5,34),(14,30)),((16,29),(18,29),(20,30)),((21,18),(31,12),(41,19)),((46,22),(48,27),(47,32)),((60,29),(63,43),(53,46)),((46,48),(26,46),(17,46))]],fill=0)

def flake(p,x,y,r=5):
    for n in range(3):
        a=n*math.pi/3
        p.line([(x-math.cos(a)*r,y-math.sin(a)*r),(x+math.cos(a)*r,y+math.sin(a)*r)],width=2)

def icon(name,size=64):
    p=Pen(size)
    if name=='Sun':sun(p)
    elif name=='Moon':moon(p)
    elif name in ('PartlyDay','PartlyNight'):
        if name=='PartlyDay':sun(p,23,23,8.5,18)
        else:moon(p,7,-1,.73)
        cloud(p,7,True)
    elif name=='Cloud':cloud(p)
    elif name in ('Rain','Snow','Sleet','Storm'):
        cloud(p,-8)
        if name=='Rain':
            for x,y in ((20,45),(33,48),(46,45)):p.line([(x,y),(x-3,y+9)])
        elif name=='Snow':
            for x,y in ((19,48),(33,52),(47,48)):flake(p,x,y,4)
        elif name=='Sleet':
            p.line([(19,45),(16,54)]);p.line([(45,45),(42,54)]);flake(p,31,50,4)
        else:
            points=[(32,41),(24,51),(33,51),(29,61),(44,46),(35,46),(38,41),(32,41)]
            p.d.polygon([(x*p.scale,y*p.scale) for x,y in points],fill=0);p.line(points,2.2)
    elif name in ('Fog','Dust'):
        cloud(p,-9)
        for x1,x2,y in ((10,52,44),(17,57,51),(7,45,58)):
            if name=='Fog':p.line([(x1,y),(x2,y)],2.4)
            else:
                for x in range(x1,x2,8):p.circle(x,y,1.2,width=1,fill=255)
    elif name=='Wind':
        p.path((9,22),[((17,22),(30,22),(38,22)),((48,22),(48,9),(40,9)),((36,9),(34,12),(34,15))])
        p.path((5,32),[((18,32),(43,32),(50,32)),((61,32),(61,18),(52,18))])
        p.path((12,42),[((21,42),(31,42),(38,42)),((48,42),(48,55),(40,55)),((36,55),(34,52),(34,49))])
    elif name=='Cold':
        for n in range(6):
            a=n*math.pi/3
            p.line([(32,32),(32+math.cos(a)*25,32+math.sin(a)*25)],2.5)
            q=(32+math.cos(a)*16,32+math.sin(a)*16)
            for sign in (-1,1):
                b=a+sign*math.pi/3
                p.line([q,(q[0]+math.cos(b)*8,q[1]+math.sin(b)*8)],2.3)
    else:
        p.circle(32,32,24)
        p.path((25,23),[((27,15),(40,16),(40,25)),((40,31),(32,31),(32,37))],2.8)
        p.circle(32,45,1.5,fill=255)
    # Supersample only during authoring. The final asset is strictly one bit.
    im=p.im.resize((size,size),Image.Resampling.LANCZOS).point(lambda v:255 if v>=128 else 0,mode='1')
    before=im.getbbox();assert before and min(before)>0 and max(before)<size,(name,size,before)
    crop=im.crop(before);out=Image.new('1',(size,size),0)
    out.paste(crop,((size-crop.width)//2,(size-crop.height)//2))
    bbox=out.getbbox();assert abs((bbox[0]+bbox[2])/2-size/2)<=.5 and abs((bbox[1]+bbox[3])/2-size/2)<=.5
    assert sum(x.bit_count() for x in out.tobytes())==sum(x.bit_count() for x in im.tobytes())
    return out

NAMES=['Sun','Moon','PartlyDay','PartlyNight','Cloud','Rain','Snow','Sleet','Storm','Fog','Dust','Wind','Cold','Unknown']
SIZES=(64,96,112,128)

def main():
    variants={size:[icon(name,size) for name in NAMES] for size in SIZES}
    images=variants[64]
    header='''// Generated by tools/generate_weather_icons.py. Original Paperdesk line-art.
// Native 64/96/112/128px bitmaps, recentered by actual ink bounds (<=0.5px error).
// The legacy 64px bitmaps and <=64px draw path preserve home-screen appearance.
// QWeather code definitions: https://dev.qweather.com/en/docs/api/weather/weather-conditions/
// Night definitions: https://icons.qweather.com/en/icons/
#pragma once
#include <cstdint>
#include <cstring>

enum class WeatherIconKind { '''+', '.join(NAMES)+''' };
namespace paperdesk_weather {
'''
    for size,icons in variants.items():
        array='bitmaps' if size==64 else f'bitmaps{size}'
        header+=f'inline constexpr uint8_t {array}[][{size*((size+7)//8)}]={{\n'
        for name,im in zip(NAMES,icons):
            data=im.tobytes();header+='    { // '+name+'\n'
            for i in range(0,len(data),16):header+='        '+','.join('0x%02x'%b for b in data[i:i+16])+',\n'
            header+='    },\n'
        header+='};\n'
    header+='''
}
inline WeatherIconKind weather_icon_kind(int code,const char* description="",bool night=false){
    using K=WeatherIconKind;
    if(code==150)return K::Moon;
    if(code>=151&&code<=153)return K::PartlyNight;
    if(code==100||code==900)return K::Sun;
    if(code>=101&&code<=103)return K::PartlyDay;
    if(code==104)return K::Cloud;
    if(code>=302&&code<=304)return K::Storm;
    if((code>=300&&code<=318)||code==350||code==351||code==399)return K::Rain;
    if(code>=404&&code<=406)return K::Sleet;
    if((code>=400&&code<=410)||code==456||code==457||code==499)return K::Snow;
    if(code==503||code==504||code==507||code==508)return K::Dust;
    if(code>=500&&code<=515)return K::Fog;
    if(code==901)return K::Cold;
    // Old cached snapshots may predate the icon field. Unknown text stays unknown.
    const char* t=description?description:"";
    auto has=[t](const char*s){return std::strstr(t,s)!=nullptr;};
    if(has("雷"))return K::Storm;
    if(has("雨夹雪")||has("雨雪"))return K::Sleet;
    if(has("雪"))return K::Snow;
    if(has("雨"))return K::Rain;
    if(has("雾")||has("霾"))return K::Fog;
    if(has("沙")||has("尘"))return K::Dust;
    if(has("阴"))return K::Cloud;
    if(has("云"))return night?K::PartlyNight:K::PartlyDay;
    if(has("晴"))return night?K::Moon:K::Sun;
    if(has("风"))return K::Wind;
    return K::Unknown;
}
inline void draw_weather_icon(int x,int y,int size,int code,const char* description,
                              bool night,void(*pixelFn)(int,int,bool)){
    if(size<=0||!pixelFn)return;
    const int kind=static_cast<int>(weather_icon_kind(code,description,night));
    const uint8_t* bits=paperdesk_weather::bitmaps[kind];
    int source=64;
    if(size>64){
        if(size==96){source=96;bits=paperdesk_weather::bitmaps96[kind];}
        else if(size==112){source=112;bits=paperdesk_weather::bitmaps112[kind];}
        else{source=128;bits=paperdesk_weather::bitmaps128[kind];}
    }
    const int stride=(source+7)/8;
    for(int yy=0;yy<size;yy++)for(int xx=0;xx<size;xx++){
        const int sx=(xx*source+source/2)/size,sy=(yy*source+source/2)/size;
        if(bits[sy*stride+sx/8]&(0x80>>(sx&7)))pixelFn(x+xx,y+yy,true);
    }
}
'''
    (ROOT/'firmware/main/weather_icons.h').write_text(header,encoding='utf-8')
    out=ROOT/'.local/preview';out.mkdir(parents=True,exist_ok=True)
    atlas=Image.new('RGB',(7*128,2*146),'white');d=ImageDraw.Draw(atlas)
    for i,(name,im) in enumerate(zip(NAMES,images)):
        x=i%7*128;y=i//7*146
        # A centered box makes the actual perceived placement directly reviewable.
        d.rectangle((x+22,y+10,x+106,y+94),outline='#c0c0c0')
        visible=Image.eval(im.convert('L'),lambda v:255-v).resize((80,80),Image.Resampling.NEAREST)
        atlas.paste(visible,(x+24,y+12));d.text((x+6,y+108),name,fill='black')
    atlas.save(out/'weather-icons.png')
    atlas=Image.new('RGB',(7*156,6*160),'white');d=ImageDraw.Draw(atlas)
    for row,size in enumerate(SIZES[1:]):
        for i,(name,im) in enumerate(zip(NAMES,variants[size])):
            x=i%7*156;y=(row*2+i//7)*160
            left=x+(156-size)//2;top=y+4+(128-size)//2
            d.rectangle((left-1,top-1,left+size,top+size),outline='#d0d0d0')
            atlas.paste(Image.eval(im.convert('L'),lambda v:255-v),(left,top))
            d.text((x+6,y+138),f'{name} {size}px',fill='black')
    atlas.save(out/'weather-icons-large.png')
    total=sum(len(im.tobytes()) for icons in variants.values() for im in icons)
    print(f'{len(NAMES)} icons at {SIZES}; {total} bitmap bytes; all bounds centered within 0.5 px; no clipped ink')

if __name__=='__main__':main()
