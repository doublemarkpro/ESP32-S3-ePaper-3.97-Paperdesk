#include "paperdesk.h"
#include "nvs_flash.h"
#include "epaper_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>
#include <algorithm>

AppData app;
std::mutex app_mutex;
std::atomic<bool> dirty{true},force_sync{true},full_refresh{false},weather_full_refresh{false};
QueueHandle_t button_queue;
const char* json_string(const cJSON*o,const char*k,const char*fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:fallback;}
int json_int(const cJSON*o,const char*k,int fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:fallback;}
std::string truncate_utf8(const std::string&s,size_t count){size_t i=0,n=0;while(i<s.size()&&n<count){unsigned char c=s[i];i+=c<128?1:(c&224)==192?2:(c&240)==224?3:4;n++;}return s.substr(0,std::min(i,s.size()));}
static void handle_key(View &v,int key){
    if(key==Back){
        v.page=v.page==3?0:3;v.selection=0;v.mode=v.page==3?2:0;v.details=false;return;
    }
    if(key==Up||key==Down){
        int direction=key==Down?1:-1;
        if(v.page==3){if(!v.details)v.selection=(v.selection+direction+7)%7;}
        else{v.page=(v.page+direction+3)%3;v.selection=0;}
        return;
    }
    if(key!=Press)return;
    if(v.page==0){v.page=1;v.selection=0;}
    else if(v.page==1)force_sync=true;
    else if(v.page==2){
        std::lock_guard<std::mutex> lock(app_mutex);
        auto j=cJSON_Parse(app.snapshot.c_str());auto c=cJSON_GetObjectItem(j,"codex");
        int n=cJSON_GetArraySize(cJSON_GetObjectItem(c,"windows"));cJSON_Delete(j);
        v.selection=n?(v.selection+1)%n:0;force_sync=true;
    }else if(v.details)v.details=false;
    else if(v.selection==0)v.details=true;
    else{
        std::lock_guard<std::mutex> lock(app_mutex);
        if(v.selection==1){app.config.home_style=app.config.home_style?0:1;full_refresh=true;}
        if(v.selection==2){app.config.weather_style=app.config.weather_style?0:1;weather_full_refresh=true;}
        if(v.selection==3)app.config.default_page=(app.config.default_page+1)%3;
        if(v.selection==4)app.config.rotation_minutes=app.config.rotation_minutes?0:5;
        if(v.selection==5)app.config.night=!app.config.night;
        if(v.selection==6)force_sync=true;
        else config_save();
    }
}
extern "C" void app_main(){
    setenv("TZ","CST-8",1);tzset();esp_err_t e=nvs_flash_init();if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();}ESP_ERROR_CHECK(e);config_load();button_queue=xQueueCreate(32,sizeof(int));board_init();sample_environment();network_start();epaper_port_init();
    View view;view.page=app.config.default_page;bool first=true;int refreshes=0;int64_t last_sample=esp_timer_get_time(),last_interaction=last_sample;time_t last_minute=0;
    ESP_LOGI("paperdesk","Paperdesk 0.4.0 ready; rocker GPIO4/5/6; protected CH340 is never used");
    for(;;){int key;if(xQueueReceive(button_queue,&key,pdMS_TO_TICKS(100))==pdTRUE){handle_key(view,key);while(xQueueReceive(button_queue,&key,0)==pdTRUE)handle_key(view,key);last_interaction=esp_timer_get_time();dirty=true;}
        int64_t now=esp_timer_get_time();if(now-last_sample>=60000000){sample_environment();last_sample=now;dirty=true;}time_t minute=time(nullptr)/60;if(minute!=last_minute){last_minute=minute;dirty=true;}
        Config cfg;{std::lock_guard<std::mutex> lock(app_mutex);cfg=app.config;}tm local;time_t epoch=time(nullptr);localtime_r(&epoch,&local);bool quiet=cfg.night&&(local.tm_hour>=23||local.tm_hour<7);
        if(cfg.rotation_minutes&&view.page<3&&!quiet&&now-last_interaction>=int64_t(cfg.rotation_minutes)*60000000){view.page=(view.page+1)%3;view.details=false;view.selection=0;last_interaction=now;dirty=true;}
        // Each theme-change request stays pending until its own page is drawn.
        // Returning home must not consume a pending weather-page full refresh.
        if(dirty.exchange(false)){bool full=(view.page==0&&full_refresh.exchange(false))||(view.page==1&&weather_full_refresh.exchange(false))||first||refreshes>=12;draw_screen(view,full);first=false;refreshes=full?0:refreshes+1;}
    }
}
