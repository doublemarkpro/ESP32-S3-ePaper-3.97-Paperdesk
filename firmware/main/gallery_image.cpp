#include "gallery_image.h"
#include "tjpgd.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace gallery_image {
namespace {
constexpr size_t MaxGrayBytes=1024*1024;
constexpr size_t WorkBytes=8192;
using Memory=std::unique_ptr<uint8_t,decltype(&std::free)>;
struct Decode {
    const uint8_t* data=nullptr;
    size_t size=0,offset=0;
    uint8_t* gray=nullptr;
    int width=0,height=0;
    size_t painted=0;
};
size_t input(JDEC* decoder,uint8_t* destination,size_t bytes) {
    auto& d=*static_cast<Decode*>(decoder->device);
    size_t count=std::min(bytes,d.size-d.offset);
    if(destination&&count)std::memcpy(destination,d.data+d.offset,count);
    d.offset+=count;
    return count;
}
int output(JDEC* decoder,void* bitmap,JRECT* rect) {
    auto& d=*static_cast<Decode*>(decoder->device);
    if(!bitmap||!rect||rect->right<rect->left||rect->bottom<rect->top||
       rect->right>=d.width||rect->bottom>=d.height)return 0;
    int width=rect->right-rect->left+1;
    const auto* pixels=static_cast<const uint8_t*>(bitmap);
    for(int y=rect->top;y<=rect->bottom;++y){
        std::memcpy(d.gray+size_t(y)*d.width+rect->left,pixels,width);
        pixels+=width;d.painted+=width;
    }
    return 1;
}
bool fail(std::string& error,const char* text){error=text;return false;}
bool header_valid(const uint8_t* data,size_t size,std::string& error){
    size_t pos=2;int components=0;
    while(pos+4<=size){
        if(data[pos++]!=0xff)return fail(error,"图片头已损坏");
        while(pos<size&&data[pos]==0xff)++pos;
        if(pos+3>size)return fail(error,"图片头不完整");
        uint8_t marker=data[pos++];
        size_t length=(size_t(data[pos])<<8)|data[pos+1];
        if(length<2||length>size-pos)return fail(error,"图片头不完整");
        if(marker==0xc0){
            if(components||length<8)return fail(error,"图片头已损坏");
            components=data[pos+7];
            if((components!=1&&components!=3)||data[pos+2]!=8)
                return fail(error,"图片格式暂不支持");
            if(length!=size_t(8+3*components))return fail(error,"图片头已损坏");
            int height=(int(data[pos+3])<<8)|data[pos+4];
            int width=(int(data[pos+5])<<8)|data[pos+6];
            if(width<1||height<1||width>MaxDimension||height>MaxDimension)
                return fail(error,"图片尺寸超出限制");
        }else if(marker>=0xc1&&marker<=0xcf&&marker!=0xc4&&marker!=0xc8&&marker!=0xcc){
            return fail(error,"图片格式暂不支持");
        }else if(marker==0xda){
            if(!components||length!=size_t(6+2*components)||data[pos+2]!=components)
                return fail(error,"图片头已损坏");
            if(data[pos+3+2*components]!=0||data[pos+4+2*components]!=63||data[pos+5+2*components]!=0)
                return fail(error,"图片格式暂不支持");
            return true;
        }
        pos+=length;
    }
    return fail(error,"图片头不完整");
}
const char* jpeg_error(JRESULT status){
    if(status==JDR_FMT2||status==JDR_FMT3)return "图片格式暂不支持";
    if(status==JDR_MEM1||status==JDR_MEM2)return "图片解码内存不足";
    return "图片不完整或已损坏";
}
// Pixel-center bilinear sampling, in fixed point, retains thin artwork lines.
int sample(const uint8_t* gray,int sw,int sh,int x,int y,int dw,int dh){
    int sx=std::clamp(int((int64_t(2*x+1)*sw*256)/(2*dw))-128,0,(sw-1)*256);
    int sy=std::clamp(int((int64_t(2*y+1)*sh*256)/(2*dh))-128,0,(sh-1)*256);
    int x0=sx>>8,y0=sy>>8,x1=std::min(x0+1,sw-1),y1=std::min(y0+1,sh-1);
    int fx=sx&255,fy=sy&255;
    int top=gray[size_t(y0)*sw+x0]*(256-fx)+gray[size_t(y0)*sw+x1]*fx;
    int bottom=gray[size_t(y1)*sw+x0]*(256-fx)+gray[size_t(y1)*sw+x1]*fx;
    return (top*(256-fy)+bottom*fy+32768)>>16;
}
std::array<uint8_t,256> tone_curve(const uint8_t* gray,size_t size){
    std::array<uint32_t,256> histogram{};
    for(size_t i=0;i<size;++i)++histogram[gray[i]];
    auto percentile=[&](int percent){
        size_t cumulative=0,target=(size*percent+99)/100;
        for(int value=0;value<256;++value){cumulative+=histogram[value];if(cumulative>=target)return value;}
        return 255;
    };
    int low=percentile(1),high=percentile(99),middle=percentile(50);
    std::array<uint8_t,256> curve{};
    for(int i=0;i<256;++i)curve[i]=uint8_t(i);
    // Do not stretch flat fields, paper-only images, or low-contrast textures.
    if(high-low<64)return curve;
    int black=std::min(low,12),white=std::max(high,128);
    if(middle>=205){
        // A concentrated bright peak is usually the photographed paper tone.
        // Keep a four-level margin above it so faint pencil lines survive.
        int peak=205;
        for(int i=206;i<=250;++i)if(histogram[i]>histogram[peak])peak=i;
        size_t paper=0;
        for(int i=std::max(0,peak-8);i<=std::min(255,peak+8);++i)paper+=histogram[i];
        if(paper*100>=size*35)white=std::min(white,std::max(220,peak+4));
    }
    for(int i=0;i<256;++i)curve[i]=uint8_t(std::clamp(((i-black)*255+(white-black)/2)/(white-black),0,255));
    return curve;
}
}

bool decode(const uint8_t* data,size_t size,std::vector<uint8_t>& mono,std::string& error){
    error.clear();
    if(!data||size<4||size>MaxJpegBytes)return fail(error,"图片大小超出限制");
    if(data[0]!=0xff||data[1]!=0xd8)return fail(error,"图片不是 JPEG 格式");
    if(!header_valid(data,size,error))return false;
    // A transfer cut off after the last MCU can otherwise look valid to TJpgDec.
    bool ended=false;
    for(size_t i=size-1;i>1;--i)if(data[i-1]==0xff&&data[i]==0xd9){ended=true;break;}
    if(!ended)return fail(error,"图片下载不完整");
    Memory work(static_cast<uint8_t*>(std::malloc(WorkBytes)),&std::free);
    if(!work)return fail(error,"图片解码内存不足");
    JDEC decoder{};Decode d;d.data=data;d.size=size;
    JRESULT status=jd_prepare(&decoder,input,work.get(),WorkBytes,&d);
    if(status!=JDR_OK)return fail(error,jpeg_error(status));
    if(!decoder.width||!decoder.height||decoder.width>MaxDimension||decoder.height>MaxDimension)
        return fail(error,"图片尺寸超出限制");
    int scale=0;
    while(scale<3&&(std::max(decoder.width,decoder.height)>>scale)>1024&&
          (decoder.width>>(scale+1))>0&&(decoder.height>>(scale+1))>0)++scale;
    d.width=decoder.width>>scale;d.height=decoder.height>>scale;
    size_t gray_size=size_t(d.width)*d.height;
    if(!gray_size||gray_size>MaxGrayBytes)return fail(error,"图片尺寸超出限制");
    Memory gray(static_cast<uint8_t*>(std::malloc(gray_size)),&std::free);
    Memory result(static_cast<uint8_t*>(std::malloc(Bytes)),&std::free);
    if(!gray||!result)return fail(error,"图片解码内存不足");
    d.gray=gray.get();
    status=jd_decomp(&decoder,output,uint8_t(scale));
    if(status!=JDR_OK||d.painted!=gray_size)return fail(error,jpeg_error(status));
    work.reset();
    auto curve=tone_curve(gray.get(),gray_size);
    std::memset(result.get(),0xff,Bytes);
    int width=Width,height=Height;
    if(int64_t(d.width)*Height>=int64_t(d.height)*Width)
        height=std::max(1,int((int64_t(d.height)*Width+d.width/2)/d.width));
    else width=std::max(1,int((int64_t(d.width)*Height+d.height/2)/d.height));
    int left=(Width-width)/2,top=(Height-height)/2;
    // Error terms use 1/16 luminance units. Serpentine traversal avoids a
    // pronounced diagonal texture; only two output-width rows are needed.
    std::array<int,Width+2> current{},next{};
    for(int y=0;y<height;++y){
        int step=y%2?-1:1;
        for(int x=step>0?0:width-1;x>=0&&x<width;x+=step){
            int luminance=curve[sample(gray.get(),d.width,d.height,x,y,width,height)];
            int old=std::clamp(luminance*16+current[x+1],0,4080);
            int level=old>=2048?4080:0;
            if(!level){size_t bit=size_t(y+top)*Width+x+left;result.get()[bit/8]&=uint8_t(~(0x80>>(bit%8)));}
            int error_value=old-level;
            current[x+1+step]+=error_value*7/16;
            next[x+1-step]+=error_value*3/16;
            next[x+1]+=error_value*5/16;
            next[x+1+step]+=error_value/16;
        }
        current=next;next.fill(0);
    }
    gray.reset();
    // Release the large transient image before allocating the small final view.
    mono.assign(result.get(),result.get()+Bytes);
    return true;
}
}
