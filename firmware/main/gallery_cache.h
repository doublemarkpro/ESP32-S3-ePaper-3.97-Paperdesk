#pragma once
#include "gallery_image.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace gallery_cache {
struct Record {
    uint32_t object_id=0, day=0;
    int64_t updated_at=0;
    std::vector<uint8_t> pixels;
};
constexpr size_t Header=24, Size=Header+gallery_image::Bytes;
inline void put(std::vector<uint8_t>& b,size_t at,uint64_t value,int bytes) {
    for(int i=0;i<bytes;++i)b[at+i]=uint8_t(value>>(8*i));
}
inline uint64_t get(const std::vector<uint8_t>& b,size_t at,int bytes) {
    uint64_t v=0;for(int i=0;i<bytes;++i)v|=uint64_t(b[at+i])<<(8*i);return v;
}
inline uint32_t checksum(const std::vector<uint8_t>& b) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<b.size();++i){
        if(i>=20&&i<24)continue;
        crc^=b[i];for(int k=0;k<8;++k)crc=(crc>>1)^(0xedb88320u&uint32_t(-int32_t(crc&1)));
    }
    return ~crc;
}
inline std::vector<uint8_t> encode(const Record& r) {
    if(r.pixels.size()!=gallery_image::Bytes||!r.object_id||r.updated_at<1704067200)return {};
    std::vector<uint8_t> b(Size);b[0]='P';b[1]='G';b[2]='A';b[3]='1';
    put(b,4,r.object_id,4);put(b,8,r.day,4);put(b,12,uint64_t(r.updated_at),8);
    std::copy(r.pixels.begin(),r.pixels.end(),b.begin()+Header);put(b,20,checksum(b),4);return b;
}
inline bool decode(const std::vector<uint8_t>& b,Record& output) {
    if(b.size()!=Size||b[0]!='P'||b[1]!='G'||b[2]!='A'||b[3]!='1'||get(b,20,4)!=checksum(b))return false;
    Record r;r.object_id=uint32_t(get(b,4,4));r.day=uint32_t(get(b,8,4));r.updated_at=int64_t(get(b,12,8));
    if(!r.object_id||r.day<20240101||r.day>99991231||r.updated_at<1704067200)return false;
    r.pixels.assign(b.begin()+Header,b.end());output=std::move(r);return true;
}
}
