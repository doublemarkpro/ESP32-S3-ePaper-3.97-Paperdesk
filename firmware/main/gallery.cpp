#include "gallery.h"
#include "gallery_catalog.h"
#include "gallery_cache.h"
#include "gallery_image.h"
#include "paperdesk.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace paperdesk_gallery {
std::atomic<bool> full_refresh{false};
namespace {
constexpr size_t Count=sizeof(artworks)/sizeof(artworks[0]);
constexpr size_t MaxJpeg=768*1024;
std::mutex mutex;
Snapshot current;
std::atomic<bool> requested{false},working{false};
std::atomic<int64_t> last_request{0};
int saved_day=0;
nvs_handle_t cache_handle=0;

int date_key(time_t now) {
    tm local={};localtime_r(&now,&local);
    return (local.tm_year+1900)*10000+(local.tm_mon+1)*100+local.tm_mday;
}
int day_index(time_t now) {
    // Chinese local calendar day, stable across restarts and year boundaries.
    return int(((int64_t(now)+8*3600)/86400)%Count);
}
void status(const std::string& value,bool busy) {
    {std::lock_guard<std::mutex> lock(mutex);if(current.status==value&&current.busy==busy)return;current.status=value;current.busy=busy;}
    dirty=true;
}
void metadata(Snapshot& state,int index) {
    const auto& a=artworks[index];state.index=index;state.count=Count;
    state.object_id=a.object_id;state.title=a.title;state.artist=a.artist;
    state.date=a.date;state.source_url=a.source_url;
}
bool load_cache() {
    if(nvs_open_from_partition("storage","gallery",NVS_READWRITE,&cache_handle)!=ESP_OK)return false;
    size_t size=0;if(nvs_get_blob(cache_handle,"picture",nullptr,&size)!=ESP_OK||size!=gallery_cache::Size)return false;
    std::vector<uint8_t> blob(size);if(nvs_get_blob(cache_handle,"picture",blob.data(),&size)!=ESP_OK)return false;
    gallery_cache::Record record;if(!gallery_cache::decode(blob,record))return false;
    int index=-1;for(size_t i=0;i<Count;++i)if(artworks[i].object_id==int(record.object_id)){index=int(i);break;}
    if(index<0)return false;
    metadata(current,index);current.pixels=std::move(record.pixels);current.updated_at=record.updated_at;
    current.ready=true;current.cached=true;current.status="已载入保存的画作";saved_day=int(record.day);return true;
}
bool save_cache(const Snapshot& state,int day) {
    if(!cache_handle)return false;
    gallery_cache::Record record;record.object_id=state.object_id;record.day=day;
    record.updated_at=state.updated_at;record.pixels=state.pixels;
    auto blob=gallery_cache::encode(record);
    return !blob.empty()&&nvs_set_blob(cache_handle,"picture",blob.data(),blob.size())==ESP_OK&&nvs_commit(cache_handle)==ESP_OK;
}
bool download(const char* url,std::vector<uint8_t>& output,std::string& error) {
    // Only the museum's verified image host is used; no account or gateway token is sent.
    constexpr const char* prefix="https://images.metmuseum.org/";
    if(strncmp(url,prefix,strlen(prefix))||strpbrk(url,"\r\n")){error="图片地址无效";return false;}
    esp_http_client_config_t config={};config.url=url;config.timeout_ms=15000;
    config.crt_bundle_attach=esp_crt_bundle_attach;config.buffer_size=4096;config.disable_auto_redirect=true;
    auto client=esp_http_client_init(&config);if(!client){error="网络内存不足";return false;}
    esp_http_client_set_header(client,"Accept","image/jpeg");
    esp_http_client_set_header(client,"Accept-Encoding","identity");
    esp_http_client_set_header(client,"User-Agent","Paperdesk/0.6.0 (ESP32 daily art display)");
    bool ok=false;std::vector<uint8_t> body;
    if(esp_http_client_open(client,0)==ESP_OK){
        int64_t length=esp_http_client_fetch_headers(client);
        int code=esp_http_client_get_status_code(client);
        if(code==200&&length>=0&&length<=int64_t(MaxJpeg)){
            body.reserve(length>0?size_t(length):MaxJpeg);
            char chunk[4096];int64_t deadline=esp_timer_get_time()+45000000;
            while(esp_timer_get_time()<deadline){
                int n=esp_http_client_read(client,chunk,sizeof chunk);
                if(n<0){error="图片传输中断";break;}
                if(n==0){ok=esp_http_client_is_complete_data_received(client);break;}
                if(body.size()+size_t(n)>MaxJpeg){error="图片超出大小限制";break;}
                body.insert(body.end(),chunk,chunk+n);vTaskDelay(1);
            }
            if(!ok&&error.empty())error="图片未下载完整";
        }else error=code==200?"图片超出大小限制":"图片服务暂不可用";
    }else error="无法连接图片服务";
    esp_http_client_close(client);esp_http_client_cleanup(client);
    if(ok&&body.size()>=4&&body[0]==0xff&&body[1]==0xd8){output=std::move(body);return true;}
    if(error.empty())error="返回内容不是 JPEG 图片";
    return false;
}
bool fetch(int first,int day) {
    status("正在直连博物馆取画…",true);
    std::string error;
    for(int attempt=0;attempt<std::min(3,int(Count));++attempt){
        const int index=(first+attempt)%Count;
        std::vector<uint8_t> jpeg,pixels;error.clear();
        if(!download(artworks[index].image_url,jpeg,error))continue;
        if(!gallery_image::decode(jpeg.data(),jpeg.size(),pixels,error))continue;
        Snapshot fresh;metadata(fresh,index);fresh.ready=true;fresh.updated_at=time(nullptr);
        fresh.pixels=std::move(pixels);fresh.status="已从博物馆直连更新";
        if(!save_cache(fresh,day))fresh.status="画作已更新 · 本次未能保存";
        {std::lock_guard<std::mutex> lock(mutex);current=std::move(fresh);saved_day=day;}
        full_refresh=true;dirty=true;
        ESP_LOGI("gallery","object=%d ready, mono=%u bytes",artworks[index].object_id,unsigned(gallery_image::Bytes));
        return true;
    }
    {std::lock_guard<std::mutex> lock(mutex);current.busy=false;current.cached=current.ready;
        current.status=current.ready?"取画失败 · 保留上幅":error.empty()?"取画失败，请稍后重试":error;}
    dirty=true;ESP_LOGW("gallery","download/decode failed; previous picture retained");return false;
}
void task(void*) {
    int64_t retry_at=0;unsigned failures=0;bool was_connected=false;
    for(;;){
        bool connected;{std::lock_guard<std::mutex> lock(app_mutex);connected=app.connected;}
        bool manual=requested.exchange(false);time_t now=time(nullptr);int64_t monotonic=esp_timer_get_time();
        if(connected&&!was_connected)retry_at=0;
        if(!connected){
            if(was_connected||manual){status(current.ready?"离线 · 保留已下载画作":"连接 Wi-Fi 后自动取画",false);}
        }else if(now<1704067200){status("等待校时后自动取画",false);}
        else{
            bool due=date_key(now)!=saved_day;
            if(manual||(due&&monotonic>=retry_at)){
                working=true;int index;
                {std::lock_guard<std::mutex> lock(mutex);index=manual&&current.index>=0?(current.index+1)%Count:day_index(now);}
                bool success=fetch(index,date_key(now));working=false;
                failures=success?0:std::min(failures+1,4u);
                retry_at=esp_timer_get_time()+(success?60000000LL:int64_t(std::min(3600u,300u<<failures))*1000000);
            }
        }
        was_connected=connected;vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}
void start() {
    {std::lock_guard<std::mutex> lock(mutex);load_cache();current.count=Count;}full_refresh=true;
    if(xTaskCreate(task,"gallery",12288,nullptr,1,nullptr)!=pdPASS)status("画廊启动失败，请重启",false);
}
void request_next() {
    int64_t now=esp_timer_get_time(),previous=last_request.load();
    if(working.load()||(previous&&now-previous<10000000))return;
    if(!last_request.compare_exchange_strong(previous,now))return;
    requested=true;
}
Snapshot snapshot(bool include_pixels) {
    std::lock_guard<std::mutex> lock(mutex);Snapshot out;
    out.ready=current.ready;out.busy=current.busy;out.cached=current.cached;
    out.index=current.index;out.count=current.count;out.object_id=current.object_id;out.updated_at=current.updated_at;
    out.title=current.title;out.artist=current.artist;out.date=current.date;out.source_url=current.source_url;out.status=current.status;
    if(include_pixels)out.pixels=current.pixels;
    return out;
}
}
