#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace paperdesk_gallery {
struct Snapshot {
    bool ready=false, busy=false, cached=false;
    int index=-1, count=0, object_id=0;
    time_t updated_at=0;
    std::string title, artist, date, source_url;
    std::string status="连接 Wi-Fi 后自动取画";
    std::vector<uint8_t> pixels;
};
// Artwork downloads and persistent cache are independent of the PC gateway.
void start();
void request_next();
Snapshot snapshot(bool include_pixels=false);
extern std::atomic<bool> full_refresh;
}
