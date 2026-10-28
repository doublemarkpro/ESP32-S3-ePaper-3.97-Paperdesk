#include "gallery_image.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc,char** argv){
    if(argc!=3)return 64;
    std::ifstream input(argv[1],std::ios::binary);
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(input)),{});
    const std::vector<uint8_t> old={0x31,0x58,0x7f};
    std::vector<uint8_t> image=old;
    std::string error;
    if(!gallery_image::decode(jpeg.data(),jpeg.size(),image,error)){
        if(image!=old||error.empty())return 3;
        std::cerr<<error<<"\n";return 2;
    }
    if(image.size()!=gallery_image::Bytes||!error.empty())return 4;
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(image.data()),image.size());
    return output.good()?0:5;
}
