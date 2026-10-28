#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gallery_image {
constexpr int Width=432;
constexpr int Height=544;
constexpr size_t Bytes=size_t(Width)*Height/8;
constexpr size_t MaxJpegBytes=768*1024;
constexpr int MaxDimension=4096;
// Baseline JPEG -> centered aspect-fit image. Row-major MSB-first, 1=white.
// Errors and unsupported JPEG variants leave the caller's old image intact.
bool decode(const uint8_t* data,size_t size,std::vector<uint8_t>& mono,std::string& error);
}
