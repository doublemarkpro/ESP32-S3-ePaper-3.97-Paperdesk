#include "paperdesk.h"
#include "gallery.h"
#include "gallery_image.h"
#include "gallery_catalog.h"
#include "garmin.h"
#include <fstream>
#include <algorithm>
#include <cstdio>
AppData app;
std::mutex app_mutex;
std::atomic<bool> dirty{false},force_sync{false},full_refresh{false},weather_full_refresh{false},calendar_full_refresh{false};
time_t preview_now=0;
QueueHandle_t button_queue=nullptr;
bool epaper_timed_out=false;
static std::string output;
namespace paperdesk_gallery {
Snapshot preview;
Snapshot snapshot(bool){return preview;}
}
namespace paperdesk_garmin {
Snapshot preview;
Snapshot snapshot(){return preview;}
}
static time_t at_date(int year,int month,int day){tm t={};t.tm_year=year-1900;t.tm_mon=month-1;t.tm_mday=day;t.tm_hour=11;t.tm_min=48;t.tm_isdst=-1;return mktime(&t);}
const char* json_string(const cJSON*o,const char*k,const char*fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsString(v)?v->valuestring:fallback;}
int json_int(const cJSON*o,const char*k,int fallback){auto v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:fallback;}
void EPD_Init(){}
void EPD_Display_Base(const uint8_t*buffer){std::ofstream f(output,std::ios::binary);f<<"P4\n480 800\n";for(int y=0;y<800;y++){for(int b=0;b<60;b++){uint8_t v=0;for(int bit=0;bit<8;bit++){int x=b*8+bit,nx=y,ny=479-x;if(!(buffer[ny*100+nx/8]&(128>>(nx&7))))v|=128>>bit;}f.put(char(v));}}}
void EPD_Display_Partial(const uint8_t*p,int,int,int,int){EPD_Display_Base(p);}
int main(int argc,char**argv){
    if(argc<2||argc>3)return 1;
    preview_now=at_date(2026,10,28);
    app.ap_name="Paperdesk-DEMO";app.ap_password="sampleonly";app.ap_active=true;
    app.config.admin_token="PREVIEW";app.env_ok=true;app.temperature=27.9;app.humidity=30;app.rtc_ok=true;
    for(int mode=0;mode<2;mode++){
        if(mode==1){
            app.connected=true;app.ip="192.168.1.100";app.gateway_error.clear();app.gateway_updated=time(nullptr);
            app.snapshot=R"({"schema":1,"weather":{"ok":true,"city":"青岛","temp":"20","text":"晴","icon":"100","feels_like":"20","wind":"东北风","wind_scale":"2","humidity":"64","updated_at":1793158080,"daily":[{"date":"2026-10-28","icon":"100","text":"晴","min":"14","max":"20"},{"date":"2026-10-29","icon":"305","text":"小雨","min":"13","max":"18"},{"date":"2026-10-30","icon":"101","text":"多云","min":"12","max":"19"}]},"codex":{"ok":true,"updated_at":1793158080,"windows":[{"bucket":"codex","label":"7天窗口","used":29,"remaining":71,"resets_at":1793660829}]}})";
        }
        for(int page=0;page<=SettingsPage;page++){
            View v;v.page=page;output=std::string(argv[1])+"/"+(mode?"configured-":"unconfigured-")+std::to_string(page)+".pbm";draw_screen(v,true);
        }
    }
    for(int style=0;style<2;style++){app.config.home_style=style;View v;output=std::string(argv[1])+"/home-style-"+(style?"b":"a")+".pbm";draw_screen(v,true);}
    {View v;v.page=GarminPage;auto& g=paperdesk_garmin::preview;
        auto render=[&](const char* name){output=std::string(argv[1])+"/garmin-"+name+".pbm";draw_screen(v,true);};
        render("unconfigured");g.configured=true;g.busy=true;g.status="正在读取云端数据";render("loading");
        g.ready=true;g.busy=false;g.date="2026-10-28";g.fetched_at=preview_now;g.last_sync=preview_now-600;
        g.steps=6842;g.step_goal=10000;g.body_battery=76;g.resting_hr=52;g.stress=24;g.hrv=68;g.sleep_seconds=7*3600+42*60;g.sleep_score=84;
        g.activity_type="running";g.activity_date="2026-10-27";g.activity_distance_m=5200;g.activity_duration_s=28*60+12;
        g.status="已直连佳明更新";render("brief");
        g.cached=true;g.status="佳明授权失效，请重新授权";render("reauthorize");
        time_t known_sync=g.last_sync;g.last_sync=0;render("reauthorize-unknown-sync");g.last_sync=known_sync;
        g.status="佳明请求受限，30分钟后重试";render("rate-limited");
        g.status="部分更新失败，30分钟后重试";render("retry");
        g.status="部分指标待同步 · 保留可用数据";render("partial");
        g.cached=true;g.date="2026-10-27";g.last_sync-=86400;app.connected=false;render("offline");app.connected=true;
        g={};g.configured=g.ready=true;g.date="2026-10-28";g.fetched_at=preview_now;g.steps=0;render("missing");
        g.body_battery=100;g.steps=123456;g.step_goal=100000;g.resting_hr=120;g.stress=100;g.hrv=180;g.sleep_seconds=12*3600+59*60;g.sleep_score=100;g.last_sync=preview_now;
        g.activity_type="trail_running";g.activity_date="2026-10-28";g.activity_distance_m=123456;g.activity_duration_s=13*3600+42*60+12;render("edge");
        g={};
    }
    {View v;v.page=GalleryPage;auto& g=paperdesk_gallery::preview;g.count=paperdesk_gallery::artwork_count;
        output=std::string(argv[1])+"/gallery-empty.pbm";draw_screen(v,true);
        g.busy=true;g.status="正在直连博物馆取画…";output=std::string(argv[1])+"/gallery-loading.pbm";draw_screen(v,true);
        if(argc==3){
            std::ifstream file(argv[2],std::ios::binary);std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)),{});std::string error;
            if(!gallery_image::decode(jpeg.data(),jpeg.size(),g.pixels,error)){fprintf(stderr,"%s\n",error.c_str());return 2;}
            int selected=0;for(size_t i=0;i<paperdesk_gallery::artwork_count;++i)if(std::string(argv[2]).find(std::to_string(paperdesk_gallery::artworks[i].object_id))!=std::string::npos){selected=int(i);break;}
            const auto& art=paperdesk_gallery::artworks[selected];g.ready=true;g.busy=false;g.index=selected;g.object_id=art.object_id;
            g.title=art.title;g.artist=art.artist;g.date=art.date;g.source_url=art.source_url;
            output=std::string(argv[1])+"/gallery.pbm";draw_screen(v,true);
            app.connected=false;g.cached=true;output=std::string(argv[1])+"/gallery-offline.pbm";draw_screen(v,true);app.connected=true;
        }
    }
    {View calendar;calendar.page=2;
        output=std::string(argv[1])+"/calendar.pbm";draw_screen(calendar,true);
        calendar.selection=1;output=std::string(argv[1])+"/calendar-next.pbm";draw_screen(calendar,true);
        const struct {const char* name;int year,month,day,offset;} cases[]={
            {"four-rows",2027,2,15,0},{"six-rows",2026,11,1,0},{"spring-festival",2026,2,17,0},
            {"leap-month",2025,7,25,0},{"leap-eleventh",2033,12,22,0},{"leap-day",2028,2,29,0},
            {"december",2026,12,31,0},{"year-boundary",2026,12,31,1},
            {"unsupported",2101,1,1,0}};
        for(auto c:cases){preview_now=at_date(c.year,c.month,c.day);calendar.selection=c.offset;output=std::string(argv[1])+"/calendar-"+c.name+".pbm";draw_screen(calendar,true);}
        preview_now=0;calendar.selection=0;output=std::string(argv[1])+"/calendar-unsynced.pbm";draw_screen(calendar,true);
        preview_now=at_date(2026,10,28);
    }
    auto weather_previews=[&](const std::string& prefix){for(int style=0;style<2;style++){app.config.weather_style=style;View page;page.page=1;output=std::string(argv[1])+"/"+prefix+(style?"-ranges":"-editorial")+".pbm";draw_screen(page,true);}};
    weather_previews("weather");
    auto j=cJSON_Parse(app.snapshot.c_str());auto w=cJSON_GetObjectItem(j,"weather");
    cJSON_SetValuestring(cJSON_GetObjectItem(w,"city"),"呼和浩特");cJSON_SetValuestring(cJSON_GetObjectItem(w,"text"),"雨夹雪");
    cJSON_SetValuestring(cJSON_GetObjectItem(w,"icon"),"404");cJSON_SetValuestring(cJSON_GetObjectItem(w,"temp"),"-12");
    cJSON_AddBoolToObject(w,"stale",true);app.temperature=-12.3;app.humidity=100;
    char* value=cJSON_PrintUnformatted(j);app.snapshot=value;cJSON_free(value);cJSON_Delete(j);
    weather_previews("weather-edge");
    View v;
    for(int style=0;style<2;style++){app.config.home_style=style;output=std::string(argv[1])+"/edge-"+(style?"b":"a")+".pbm";draw_screen(v,true);}
    for(int remaining:{0,100}){
        j=cJSON_Parse(app.snapshot.c_str());auto codex=cJSON_GetObjectItem(j,"codex");auto bucket=cJSON_GetArrayItem(cJSON_GetObjectItem(codex,"windows"),0);
        cJSON_SetNumberValue(cJSON_GetObjectItem(bucket,"remaining"),remaining);cJSON_SetNumberValue(cJSON_GetObjectItem(bucket,"used"),100-remaining);cJSON_AddBoolToObject(codex,"stale",true);
        value=cJSON_PrintUnformatted(j);app.snapshot=value;cJSON_free(value);cJSON_Delete(j);
        for(int style=0;style<2;style++){app.config.home_style=style;output=std::string(argv[1])+"/quota-"+std::to_string(remaining)+(style?"-b":"-a")+".pbm";draw_screen(v,true);}
    }
    j=cJSON_Parse(app.snapshot.c_str());w=cJSON_GetObjectItem(j,"weather");auto days=cJSON_GetObjectItem(w,"daily");
    cJSON_SetValuestring(cJSON_GetObjectItem(w,"text"),"雷阵雨伴有冰雹");cJSON_SetValuestring(cJSON_GetObjectItem(w,"temp"),"-100");
    cJSON_SetValuestring(cJSON_GetObjectItem(cJSON_GetArrayItem(days,0),"date"),"2026-10-27");
    for(int i=0;i<3;++i){auto day=cJSON_GetArrayItem(days,i);cJSON_SetValuestring(cJSON_GetObjectItem(day,"min"),"-12");cJSON_SetValuestring(cJSON_GetObjectItem(day,"max"),"-12");cJSON_SetValuestring(cJSON_GetObjectItem(day,"text"),"雷阵雨伴有冰雹");}
    value=cJSON_PrintUnformatted(j);app.snapshot=value;cJSON_free(value);cJSON_Delete(j);weather_previews("weather-equal");
    j=cJSON_Parse(app.snapshot.c_str());days=cJSON_GetObjectItem(cJSON_GetObjectItem(j,"weather"),"daily");cJSON_SetValuestring(cJSON_GetObjectItem(cJSON_GetArrayItem(days,0),"min"),"");cJSON_SetValuestring(cJSON_GetObjectItem(cJSON_GetArrayItem(days,1),"max"),"oops");cJSON_DeleteItemFromArray(days,2);
    value=cJSON_PrintUnformatted(j);app.snapshot=value;cJSON_free(value);cJSON_Delete(j);weather_previews("weather-missing");
    app.snapshot="{}";weather_previews("weather-empty");for(int style=0;style<2;style++){app.config.home_style=style;output=std::string(argv[1])+"/empty-"+(style?"b":"a")+".pbm";draw_screen(v,true);}
    app.env_ok=false;for(int style=0;style<2;style++){app.config.home_style=style;output=std::string(argv[1])+"/sensor-unavailable-"+(style?"b":"a")+".pbm";draw_screen(v,true);}
    v.page=SettingsPage;v.details=true;output=std::string(argv[1])+"/network-settings.pbm";draw_screen(v,true);
}
