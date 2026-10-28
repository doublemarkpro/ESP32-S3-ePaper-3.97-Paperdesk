#include "paperdesk.h"
#include <fstream>
#include <algorithm>
#include <cstdio>
AppData app;
std::mutex app_mutex;
std::atomic<bool> dirty{false},force_sync{false};
QueueHandle_t button_queue=nullptr;
bool epaper_timed_out=false;
static std::string output;
const char* json_string(const cJSON*o,const char*k,const char*fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:fallback;}
int json_int(const cJSON*o,const char*k,int fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:fallback;}
void EPD_Init(){}
void EPD_Display_Base(const uint8_t*buffer){std::ofstream f(output,std::ios::binary);f<<"P4\n480 800\n";for(int y=0;y<800;y++){for(int b=0;b<60;b++){uint8_t v=0;for(int bit=0;bit<8;bit++){int x=b*8+bit,nx=y,ny=479-x;if(!(buffer[ny*100+nx/8]&(128>>(nx&7))))v|=128>>bit;}f.put(char(v));}}}
void EPD_Display_Partial(const uint8_t*p,int,int,int,int){EPD_Display_Base(p);}
int main(int argc,char**argv){
    if(argc!=2)return 1;
    app.ap_name="Paperdesk-DEMO";app.ap_password="sampleonly";app.ap_active=true;
    app.config.admin_token="PREVIEW";app.env_ok=true;app.temperature=27.9;app.humidity=30;app.rtc_ok=true;
    for(int mode=0;mode<2;mode++){
        if(mode==1){
            app.connected=true;app.ip="192.168.1.100";app.gateway_error.clear();app.gateway_updated=time(nullptr);
            app.snapshot=R"({"schema":1,"weather":{"ok":true,"city":"青岛","temp":"20","text":"晴","icon":"100","feels_like":"20","wind":"东北风","wind_scale":"2","humidity":"64","updated_at":1793158080,"daily":[{"date":"2026-10-28","icon":"100","text":"晴","min":"14","max":"20"},{"date":"2026-10-29","icon":"305","text":"小雨","min":"13","max":"18"},{"date":"2026-10-30","icon":"101","text":"多云","min":"12","max":"19"}]},"codex":{"ok":true,"updated_at":1793158080,"windows":[{"bucket":"codex","label":"7天窗口","used":24,"remaining":76,"resets_at":1793660829}]}})";
        }
        for(int page=0;page<4;page++){
            View v;v.page=page;output=std::string(argv[1])+"/"+(mode?"configured-":"unconfigured-")+std::to_string(page)+".pbm";draw_screen(v,true);
        }
    }
    auto j=cJSON_Parse(app.snapshot.c_str());auto w=cJSON_GetObjectItem(j,"weather");
    cJSON_SetValuestring(cJSON_GetObjectItem(w,"city"),"呼和浩特");cJSON_SetValuestring(cJSON_GetObjectItem(w,"text"),"雨夹雪");
    cJSON_SetValuestring(cJSON_GetObjectItem(w,"icon"),"404");cJSON_SetValuestring(cJSON_GetObjectItem(w,"temp"),"-12");
    cJSON_AddBoolToObject(w,"stale",true);app.temperature=-12.3;app.humidity=100;
    char* value=cJSON_PrintUnformatted(j);app.snapshot=value;cJSON_free(value);cJSON_Delete(j);
    output=std::string(argv[1])+"/edge-cases.pbm";View v;draw_screen(v,true);
    v.page=3;v.details=true;output=std::string(argv[1])+"/network-settings.pbm";draw_screen(v,true);
}
