#pragma once
#include <string>
#include <mutex>
#include <atomic>
#include <ctime>
#ifndef PAPERDESK_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#else
using QueueHandle_t=void*;
#ifdef _WIN32
inline tm* localtime_r(const time_t* input,tm* output){localtime_s(output,input);return output;}
#endif
#endif
#include "cJSON.h"

struct Config {
    std::string ssid, wifi_password, gateway, gateway_token, admin_token;
    int default_page=0, rotation_minutes=0, home_style=0;
    bool night=true;
};
struct AppData {
    Config config;
    bool connected=false, ap_active=false, env_ok=false, rtc_ok=false;
    float temperature=0, humidity=0;
    std::string ip="192.168.4.1", ap_name, ap_password;
    std::string snapshot="{}", gateway_error="网关未配置";
    time_t gateway_updated=0;
};
extern AppData app;
extern std::mutex app_mutex;
extern std::atomic<bool> dirty, force_sync, full_refresh;
extern QueueHandle_t button_queue;
enum Key { Up=1, Press=2, Down=3, Back=4 };
struct View { int page=0, selection=0, mode=0; bool details=false; };
void board_init();
void sample_environment();
void rtc_store_system_time();
void config_load();
void config_save();
void network_start();
void draw_screen(const View &view, bool full);
const char* json_string(const cJSON* o,const char* key,const char* fallback="");
int json_int(const cJSON* o,const char* key,int fallback=0);
std::string truncate_utf8(const std::string &s,size_t count);
