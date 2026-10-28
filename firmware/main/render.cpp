#include "paperdesk.h"
#include "epaper_port.h"
#include "weather_icons.h"
#include "weather_layout.h"
#include "calendar.h"
#include "gallery.h"
#include "gallery_image.h"
#include "weight_data.h"
#include "garmin.h"
#include "esp_log.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern const uint8_t f20[] asm("_binary_font20_bin_start");
extern const uint8_t f24[] asm("_binary_font24_bin_start");
extern const uint8_t f32[] asm("_binary_font32_bin_start");
extern const uint8_t f56[] asm("_binary_font56_bin_start");
extern const uint8_t f112[] asm("_binary_font112_bin_start");
extern const uint8_t f144[] asm("_binary_font144_bin_start");
extern const uint8_t theme_a_clock[] asm("_binary_theme_a_clock_bin_start");
extern const uint8_t theme_a_large[] asm("_binary_theme_a_large_bin_start");
extern const uint8_t theme_a_value[] asm("_binary_theme_a_value_bin_start");
extern const uint8_t theme_b_clock[] asm("_binary_theme_b_clock_bin_start");
extern const uint8_t theme_b_large[] asm("_binary_theme_b_large_bin_start");
extern const uint8_t theme_b_value[] asm("_binary_theme_b_value_bin_start");
extern const uint8_t weather_serif[] asm("_binary_weather_serif_bin_start");
extern const uint8_t weather_sans[] asm("_binary_weather_sans_bin_start");
extern const uint8_t calendar_digits[] asm("_binary_calendar_digits_bin_start");
#ifdef PAPERDESK_HOST
extern time_t preview_now;
#endif
namespace {
constexpr int AClock=201, ALarge=202, AValue=203, BClock=204, BLarge=205, BValue=206;
constexpr int WeatherSerif=207, WeatherSans=208;
constexpr int CalendarDigits=209;
uint8_t frame[48000];
uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p+2)) << 16); }
const uint8_t* font(int size) {
    switch(size) { case CalendarDigits:return calendar_digits; case WeatherSerif:return weather_serif; case WeatherSans:return weather_sans; case AClock:return theme_a_clock; case ALarge:return theme_a_large; case AValue:return theme_a_value; case BClock:return theme_b_clock; case BLarge:return theme_b_large; case BValue:return theme_b_value; case 20:return f20; case 32:return f32; case 56:return f56; case 112:return f112; case 144:return f144; default:return f24; }
}
uint32_t decode(const char*& p) {
    uint8_t c=*p++; if(c<128)return c;
    int n=(c&0xe0)==0xc0?1:(c&0xf0)==0xe0?2:(c&0xf8)==0xf0?3:0;
    uint32_t value=c&((1<<(6-n))-1);
    for(int i=0;i<n;i++){if(!*p||(*p&0xc0)!=0x80)return '?';value=(value<<6)|(*p++&63);}
    return n?value:'?';
}
const uint8_t* glyph(const uint8_t* f,uint32_t cp) {
    int lo=0,hi=u16(f+2)-1;
    while(lo<=hi){int m=(lo+hi)/2;const uint8_t* g=f+4+m*8;uint32_t v=u32(g);if(v==cp)return g;if(cp<v)hi=m-1;else lo=m+1;}
    return nullptr;
}
void pixel(int x,int y,bool ink=true) {
    if(x<0||x>=480||y<0||y>=800)return;
    int nx=y,ny=479-x,index=ny*100+nx/8;uint8_t mask=128>>(nx&7);
    if(ink)frame[index]&=~mask;else frame[index]|=mask;
}
void fill(int x,int y,int w,int h,bool ink=true) {
    for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)pixel(xx,yy,ink);
}
bool inside_round(int x,int y,int w,int h,int radius) {
    if(x<0||x>=w||y<0||y>=h)return false;
    radius=std::min(radius,std::min((w-1)/2,(h-1)/2));
    int cx=std::clamp(x,radius,w-1-radius),cy=std::clamp(y,radius,h-1-radius);
    return (x-cx)*(x-cx)+(y-cy)*(y-cy)<=radius*radius;
}
void round_box(int x,int y,int w,int h,int radius,int stroke=2) {
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++)
        if(inside_round(xx,yy,w,h,radius)&&!inside_round(xx-stroke,yy-stroke,w-2*stroke,h-2*stroke,std::max(0,radius-stroke)))pixel(x+xx,y+yy);
}
void progress(int x,int y,int w,int h,int amount) {
    int end=w*std::clamp(amount,0,100)/100;
    round_box(x,y,w,h,h/2,2);
    for(int yy=0;yy<h;yy++)for(int xx=0;xx<end;xx++)if(inside_round(xx,yy,w,h,h/2))pixel(x+xx,y+yy);
}
struct Bounds {int left=0,top=0,right=-1,bottom=-1,advance=0;int w()const{return right-left+1;}int h()const{return bottom-top+1;}};
// Measure visible ink, not nominal font cells. This is also the alignment
// reference for weather icons, different font sizes, and asymmetric glyphs.
Bounds measure(const std::string& s,int size) {
    const uint8_t* f=font(size);int fs=u16(f),count=u16(f+2),stride=(fs+7)/8,x=0;
    Bounds b;b.left=INT_MAX;b.top=INT_MAX;
    const char* p=s.c_str();
    while(*p){auto g=glyph(f,decode(p));int advance=g?u16(g+4):size;
        if(g){const uint8_t* bits=f+4+count*8+((g-f-4)/8)*fs*stride;
            for(int yy=0;yy<fs;yy++)for(int xx=0;xx<fs;xx++)if(bits[yy*stride+xx/8]&(128>>(xx&7))){b.left=std::min(b.left,x+xx);b.right=std::max(b.right,x+xx);b.top=std::min(b.top,yy);b.bottom=std::max(b.bottom,yy);}
        }else{b.left=std::min(b.left,x);b.right=std::max(b.right,x+size-1);b.top=std::min(b.top,0);b.bottom=std::max(b.bottom,size-1);}
        x+=advance;
    }
    b.advance=x;if(b.right<0){b.left=b.top=0;b.bottom=-1;}return b;
}
int width(const std::string& s,int size) {
    const uint8_t* f=font(size);int w=0;const char* p=s.c_str();
    while(*p){auto g=glyph(f,decode(p));w+=g?u16(g+4):size;}return w;
}
std::string fit(const std::string& s,int size,int limit) {
    if(width(s,size)<=limit)return s;
    std::string result;const char* p=s.c_str();int available=limit-width("…",size);
    while(*p){const char* start=p;decode(p);std::string next(start,p-start);if(width(result+next,size)>available)break;result+=next;}
    return result+"…";
}
void raw_text(int x,int y,const std::string& s,int size,bool ink=true) {
    const uint8_t* f=font(size);int fs=u16(f),count=u16(f+2),stride=(fs+7)/8;const char* p=s.c_str();
    while(*p){auto g=glyph(f,decode(p));
        if(g){const uint8_t* bits=f+4+count*8+((g-f-4)/8)*fs*stride;
            for(int yy=0;yy<fs;yy++)for(int xx=0;xx<fs;xx++)if(bits[yy*stride+xx/8]&(128>>(xx&7)))pixel(x+xx,y+yy,ink);
            x+=u16(g+4);
        }else{for(int yy=0;yy<size-2;yy++)for(int xx=0;xx<size-2;xx++)if(xx<2||yy<2||xx>=size-4||yy>=size-4)pixel(x+xx,y+yy,ink);x+=size;}
    }
}
void label(int x,int ink_top,const std::string& value,int size=24,int max_width=416) {
    auto s=fit(value,size,max_width);auto b=measure(s,size);raw_text(x-b.left,ink_top-b.top,s,size);
}
void right(int edge,int ink_top,const std::string& s,int size=20,int max_width=416) {
    auto value=fit(s,size,max_width);auto b=measure(value,size);raw_text(edge-b.right,ink_top-b.top,value,size);
}
void in_box(int x,int y,int w,int h,const std::string& value,int size,bool align_left=false,bool ink=true) {
    auto s=fit(value,size,w);auto b=measure(s,size);
    raw_text((align_left?x:x+(w-b.w())/2)-b.left,y+(h-b.h())/2-b.top,s,size,ink);
}
void multiline(int x,int y,const std::string& s,int size=24,int rows=3,int w=416) {
    std::string row;const char* p=s.c_str();
    while(*p&&rows>0){const char* start=p;decode(p);std::string c(start,p-start);if(width(row+c,size)>w){label(x,y,row,size,w);y+=size+14;--rows;row.clear();}row+=c;}
    if(rows>0&&!row.empty())label(x,y,row,size,w);
}
// A shared baseline keeps °C and % attached to their numerals. Use the ink
// bounds of the complete group to center its contents inside the weather row.
void number_unit(int x,int center_y,int area_width,const std::string& number,const std::string& unit,int number_size=56,bool align_right=false,int unit_size=24) {
    while(number_size>32&&width(number,number_size)+width(unit,unit_size)+4>area_width)
        number_size=number_size==WeatherSerif?BLarge:number_size==WeatherSans?ALarge:number_size==ALarge?AValue:number_size==BLarge?BValue:32;
    auto nb=measure(number,number_size),ub=measure(unit,unit_size);
    int nbase=u16(font(number_size)+10),ubase=u16(font(unit_size)+10);
    int top=std::min(nb.top-nbase,ub.top-ubase),bottom=std::max(nb.bottom-nbase,ub.bottom-ubase);
    int baseline=center_y-(top+bottom)/2,total=nb.advance+4+ub.w();
    int start=align_right?x+area_width-total:x;
    raw_text(start,baseline-nbase,number,number_size);
    raw_text(start+nb.advance+4-ub.left,baseline-ubase,unit,unit_size);
}
std::string timestamp(time_t epoch,const char* format="%m-%d %H:%M") {
    if(epoch<1704067200)return "尚未同步";
    tm t;localtime_r(&epoch,&t);char out[64];strftime(out,sizeof(out),format,&t);return out;
}
int weather_code(const cJSON* weather,const char* key="icon") {
    auto item=cJSON_GetObjectItemCaseSensitive(weather,key);
    if(cJSON_IsNumber(item))return item->valueint;
    if(cJSON_IsString(item)&&item->valuestring[0])return atoi(item->valuestring);
    return -1;
}
const cJSON* home_bucket(const cJSON* windows) {
    const cJSON* chosen=cJSON_GetArrayItem(windows,0);
    for(int i=0;i<cJSON_GetArraySize(windows);i++){auto w=cJSON_GetArrayItem(windows,i);if(std::string(json_string(w,"label")).find("天")!=std::string::npos&&!strcmp(json_string(w,"bucket","codex"),"codex"))return w;}
    return chosen;
}
std::string quota_label(const cJSON* bucket) {
    std::string s=json_string(bucket,"label","额度");auto p=s.find("窗口");if(p!=std::string::npos)s.replace(p,6,"额度");return s;
}
void draw_home(const AppData& a,const cJSON* weather,const cJSON* codex,const cJSON* windows,
               bool weather_ok,bool quota_ok,bool weather_stale,bool quota_stale,time_t now) {
    bool editorial=a.config.home_style==1,clock_ok=now>=1704067200;
    tm local={};localtime_r(&now,&local);
    bool night=clock_ok&&(local.tm_hour<6||local.tm_hour>=18);
    const char* weekdays[]={"日","一","二","三","四","五","六"};char date[64],clock[16],temp[24],humidity[16];
    if(clock_ok){
        if(editorial)snprintf(date,sizeof(date),"%d年%d月%d日",local.tm_year+1900,local.tm_mon+1,local.tm_mday);
        else strftime(date,sizeof(date),"%Y.%m.%d",&local);
        strftime(clock,sizeof(clock),"%H:%M",&local);
        right(448,43,std::string("星期")+weekdays[local.tm_wday],20);
    }else{strcpy(date,"纸间 · 请先校时");strcpy(clock,"--:--");}
    label(32,43,date,20,320);
    in_box(32,editorial?106:94,416,editorial?190:212,clock,editorial?BClock:AClock);
    if(a.env_ok){snprintf(temp,sizeof(temp),"%.1f",a.temperature);snprintf(humidity,sizeof(humidity),"%.0f",a.humidity);}
    else{strcpy(temp,"--");strcpy(humidity,"--");}
    std::string city=json_string(weather,"city"),condition=json_string(weather,"text");
    std::string heading=weather_ok?city+" · "+condition:"天气待配置";
    auto today=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(weather,"daily"),0);
    std::string range=weather_ok?std::string(json_string(today,"min","--"))+"–"+json_string(today,"max","--")+"°C":"长按进入设置";
    if(weather_ok&&weather_stale)range+=" · 缓存";
    auto bucket=home_bucket(windows);int remaining=std::clamp(json_int(bucket,"remaining"),0,100);
    std::string window=quota_label(bucket)+(quota_stale?" · 缓存":"");
    time_t reset=json_int(bucket,"resets_at");
    if(!editorial){
        fill(32,332,416,1);
        label(32,374,heading,24,214);
        draw_weather_icon(32,430,64,weather_ok?weather_code(weather):-1,weather_ok?condition.c_str():"",night,pixel);
        number_unit(110,462,138,weather_ok?json_string(weather,"temp","--"):"--","°C",AValue);
        label(32,532,range,20,214);
        label(278,378,"室内",20);
        number_unit(278,462,170,temp,"°C",AValue);
        label(278,530,"湿度",20);number_unit(338,540,110,humidity,"%",AValue,true);
        label(32,581,"Codex",24);
        if(quota_ok){
            right(448,585,window,20,240);
            in_box(32,626,240,90,std::to_string(remaining)+"%",ALarge,true);
            right(448,679,"剩余额度",20,164);
            progress(32,726,416,8,remaining);
            label(32,753,timestamp(reset,"%m/%d %H:%M")+" 重置",20,300);
        }else{
            label(32,650,"额度暂不可用",24);
            multiline(32,704,a.config.gateway.empty()?"长按设置，连接电脑网关":json_string(codex,"error",a.gateway_error.c_str()),20,2);
        }
    }else{
        fill(248,342,1,392);
        draw_weather_icon(32,344,64,weather_ok?weather_code(weather):-1,weather_ok?condition.c_str():"",night,pixel);
        if(width(heading,24)<=132)in_box(104,344,132,64,heading,24,true);
        else{label(104,350,weather_ok?city:"天气",24,132);label(104,384,weather_ok?condition:"待配置",20,132);}
        number_unit(32,466,196,weather_ok?json_string(weather,"temp","--"):"--","°C",BValue);
        label(32,511,range,20,200);
        label(32,579,"室内",20);
        number_unit(32,635,196,temp,"°C",BValue);
        label(32,701,"湿度",20);number_unit(92,710,140,humidity,"%",BValue,true);
        label(280,349,"Codex",24,168);label(280,409,"剩余",20);
        if(quota_ok){
            number_unit(280,490,168,std::to_string(remaining),"%",BLarge);
            label(280,562,window,20,168);progress(280,607,168,8,remaining);
            label(280,652,timestamp(reset,"%m/%d"),24,168);
            label(280,698,timestamp(reset,"%H:%M")+" 重置",20,168);
        }else{
            in_box(280,450,168,80,"--",BLarge,true);
            multiline(280,570,"等待电脑网关",20,2,168);multiline(280,662,"长按进入设置",20,2,168);
        }
    }
}
// A two-line centered label preserves long condition names without tiny type.
void centered_lines(int x,int y,int w,int h,const std::string& value,int size=20) {
    if(width(value,size)<=w){in_box(x,y,w,h,value,size);return;}
    std::string first;const char* p=value.c_str();const char* rest=p;
    while(*p){const char* start=p;decode(p);std::string next(start,p-start);if(width(first+next,size)>w){rest=start;break;}first+=next;rest=p;}
    in_box(x,y,w,h/2,first,size);in_box(x,y+h/2,w,h-h/2,rest,size);
}
std::string field(const cJSON* object,const char* key) {
    std::string value=json_string(object,key);return value.empty()?"--":value;
}
std::string range_text(const weather_ui::Range& range) {
    return range.valid?std::to_string(range.low)+"–"+std::to_string(range.high)+"°C":"-- / --°C";
}
void endpoint(int x,int y) {
    for(int dy=-4;dy<=4;++dy)for(int dx=-4;dx<=4;++dx){int r=dx*dx+dy*dy;if(r<=16)pixel(x+dx,y+dy,r>=5);}
}
void centered_number(int center,int y,const std::string& value,int left=224,int right_edge=448) {
    auto b=measure(value,20);int start=std::clamp(center-b.w()/2,left,right_edge-b.w());label(start,y,value,20,right_edge-left);
}
void draw_weather(const AppData& a,const cJSON* weather,bool available,bool stale,time_t now) {
    bool editorial=a.config.weather_style==0,clock_ok=now>=1704067200;
    tm local={};localtime_r(&now,&local);
    bool night=clock_ok&&(local.tm_hour<6||local.tm_hour>=18);
    std::array<std::string,3> current_dates;
    for(int i=0;i<3&&clock_ok;++i)current_dates[i]=timestamp(now+i*86400,"%Y-%m-%d");
    label(32,40,available?field(weather,"city"):"天气",32,216);
    if(clock_ok){const char* weekdays[]={"日","一","二","三","四","五","六"};char date[48];snprintf(date,sizeof(date),"%d月%d日 周%s",local.tm_mon+1,local.tm_mday,weekdays[local.tm_wday]);right(448,49,date,20,180);}
    else right(448,49,"请先校时",20,180);
    if(editorial)fill(32,98,416,1);
    if(!available){
        draw_weather_icon(184,160,112,-1,"",false,pixel);
        in_box(32,305,416,50,"天气尚未连接",32);
        multiline(32,402,json_string(weather,"error","请在电脑网关填写和风天气信息"),24,4);
        label(32,650,"长按拨轮进入设置",20);
    }else{
        auto days=cJSON_GetObjectItemCaseSensitive(weather,"daily");
        std::array<const cJSON*,3> forecast;std::array<weather_ui::Range,3> ranges;std::array<std::string,3> names;
        for(int i=0;i<3;++i){forecast[i]=cJSON_GetArrayItem(days,i);ranges[i]=weather_ui::range(json_string(forecast[i],"min"),json_string(forecast[i],"max"));names[i]=weather_ui::day_label(json_string(forecast[i],"date"),current_dates);}
        std::string wind=field(weather,"wind"),scale=field(weather,"wind_scale");if(scale!="--")wind+=" "+scale+"级";
        if(editorial){
            number_unit(32,220,260,field(weather,"temp"),"°C",WeatherSerif,false,32);
            draw_weather_icon(320,164,112,weather_code(weather),json_string(weather,"text"),night,pixel);
            centered_lines(304,286,144,52,field(weather,"text"),24);
            in_box(304,345,144,30,range_text(ranges[0]),20);
            label(32,386,"体感 "+field(weather,"feels_like")+"°C",20,230);right(448,386,"湿度 "+field(weather,"humidity")+"%",20,170);
            label(32,420,wind,20);fill(32,456,416,1);label(32,478,"未来三天",24);
            for(int i=0;i<3;++i){int x=32+i*144;if(i)fill(x-8,520,1,190);
                in_box(x,516,128,30,names[i],20);
                draw_weather_icon(x+40,559,48,weather_code(forecast[i]),json_string(forecast[i],"text"),false,pixel);
                centered_lines(x,623,128,48,field(forecast[i],"text"),20);
                int size=width(range_text(ranges[i]),24)<=128?24:20;in_box(x,684,128,30,range_text(ranges[i]),size);
            }
        }else{
            draw_weather_icon(52,158,112,weather_code(weather),json_string(weather,"text"),night,pixel);
            centered_lines(32,293,152,60,field(weather,"text"),24);
            number_unit(218,214,230,field(weather,"temp"),"°C",WeatherSans,false,32);
            label(218,307,"体感 "+field(weather,"feels_like")+"°C",20,230);
            label(32,382,wind,20,256);right(448,382,"湿度 "+field(weather,"humidity")+"%",20,160);
            fill(32,426,416,1);label(32,450,"三日温差",24);right(448,454,"最低 / 最高 °C",20,192);
            auto axis=weather_ui::shared_axis(ranges);constexpr int left=244,right_edge=432;
            for(int i=0;i<3;++i){int cy=510+i*68;
                in_box(32,cy-16,60,32,names[i],20,true);
                draw_weather_icon(98,cy-24,48,weather_code(forecast[i]),json_string(forecast[i],"text"),false,pixel);
                centered_lines(156,cy-24,64,48,field(forecast[i],"text"),20);
                if(!ranges[i].valid){in_box(232,cy-14,216,28,"暂无数据",20);continue;}
                int lo=axis.project(ranges[i].low,left,right_edge),hi=axis.project(ranges[i].high,left,right_edge);
                fill(lo,cy-1,hi-lo+1,3);endpoint(lo,cy);endpoint(hi,cy);
                std::string low=std::to_string(ranges[i].low),high=std::to_string(ranges[i].high);
                if(ranges[i].low==ranges[i].high)centered_number(lo,cy-29,low);
                else if(hi-lo<(width(low,20)+width(high,20))/2+12)centered_number((lo+hi)/2,cy-29,low+"–"+high);
                else{centered_number(lo,cy-29,low);centered_number(hi,cy-29,high);}
            }
            if(axis.valid){
                fill(left,682,right_edge-left+1,1);
                for(int i=0;i<3;++i){int x=left+(right_edge-left)*i/2;fill(x,678,1,9);char tick[20];double value=axis.low+(axis.high-axis.low)*i/2.0;if(value==int(value))snprintf(tick,sizeof(tick),"%d",int(value));else snprintf(tick,sizeof(tick),"%.1f",value);centered_number(x,698,tick);}
            }else in_box(232,683,216,30,"暂无温度预报",20);
        }
    }
    if(available)in_box(32,734,416,28,std::string(stale?"缓存 · ":"和风天气 · ")+timestamp(json_int(weather,"updated_at"),stale?"%m/%d %H:%M":"%H:%M")+" 更新",20);
    in_box(32,768,416,28,"按下同步 · 长按设置",20);
}
void draw_calendar(const View& view,time_t now) {
    namespace cal=paperdesk_calendar;
    if(now<1704067200){
        label(32,40,"月历",32);in_box(32,220,416,60,"请先校准日期",32);
        centered_lines(32,340,416,70,"连接 Wi-Fi 自动校时，或在设备网页保存手机时间。",24);
        in_box(32,760,416,30,"长按拨轮进入设置",20);return;
    }
    tm local={};localtime_r(&now,&local);
    cal::Date today{local.tm_year+1900,local.tm_mon+1,local.tm_mday};
    cal::Date month=cal::add_months(today,view.selection==1?1:0);
    int rows=cal::month_rows(month.year,month.month),first=cal::weekday_monday0(month);
    const char* week[]={"一","二","三","四","五","六","日"};
    label(32,42,std::to_string(month.year)+"年",24,170);
    number_unit(32,158,174,std::to_string(month.month),"月",WeatherSans);
    label(32,224,view.selection==1?"下月":"本月",20);

    char today_line[64];snprintf(today_line,sizeof(today_line),"今日 %02d/%02d · 星期%s",today.month,today.day,week[cal::weekday_monday0(today)]);
    right(448,44,today_line,20,228);
    auto lunar=cal::lunar(today);
    if(lunar.valid){
        right(448,84,"农历"+cal::lunar_month_name(lunar)+cal::lunar_day_name(lunar.day),24,228);
        right(448,126,cal::ganzhi_year(lunar.year)+" · "+cal::zodiac(lunar.year)+"年",20,228);
    }else{right(448,84,"农历暂无数据",24,228);right(448,126,"公历正常显示",20,228);}
    auto next=cal::next_term(today);
    right(448,168,"下一节气",20,228);
    if(next.valid){char date[40];snprintf(date,sizeof(date)," · %d月%d日",next.date.month,next.date.day);right(448,202,std::string(next.name)+date,24,228);}
    else right(448,202,"暂无节气数据",20,228);
    fill(32,250,416,1);
    for(int column=0;column<7;++column){int x=32+column*416/7,w=(column+1)*416/7-column*416/7;in_box(x,275,w,30,week[column],20);if(column>=5)fill(x+w/2-10,310,20,1);}

    if(rows>0){
        constexpr int grid_top=320,grid_height=414;
        for(int day=1;day<=cal::days_in_month(month.year,month.month);++day){
            int slot=first+day-1,row=slot/7,column=slot%7;
            int x=32+column*416/7,w=(column+1)*416/7-column*416/7;
            int cy=grid_top+(2*row+1)*grid_height/(2*rows);
            bool selected=today.year==month.year&&today.month==month.month&&today.day==day;
            if(selected){int chip_width=48,chip_left=x+(w-chip_width)/2;for(int yy=0;yy<42;yy++)for(int xx=0;xx<chip_width;xx++)if(inside_round(xx,yy,chip_width,42,7))pixel(chip_left+xx,cy-34+yy);}
            in_box(x,cy-31,w,36,std::to_string(day),CalendarDigits,false,!selected);
            cal::Date date{month.year,month.month,day};auto lunar_day=cal::lunar(date);auto term=cal::term_on(date);
            const char* short_months[]={"","正","二","三","四","五","六","七","八","九","十","冬","腊"};
            std::string month_label=lunar_day.valid?(lunar_day.leap?std::string("闰")+short_months[lunar_day.month]:std::string(short_months[lunar_day.month])+"月"):"--";
            std::string secondary=term.valid?term.name:lunar_day.valid?(lunar_day.day==1?month_label:cal::lunar_day_name(lunar_day.day)):"--";
            in_box(x,cy+12,w,26,secondary,20);
        }
    }
    fill(32,751,416,1);in_box(32,764,416,30,"短按切换月份 · 长按设置",20);
}
void draw_gallery(const AppData& a) {
    auto gallery=paperdesk_gallery::snapshot(true);
    label(24,27,"纸间美术馆",24,300);
    if(gallery.ready)right(456,31,std::to_string(gallery.index+1)+" / "+std::to_string(gallery.count),20,140);
    else right(456,31,"每日一画",20,140);
    fill(24,64,432,1);
    if(gallery.ready&&gallery.pixels.size()==gallery_image::Bytes){
        for(int y=0;y<gallery_image::Height;++y)for(int x=0;x<gallery_image::Width;++x)
            if(!(gallery.pixels[size_t(y)*gallery_image::Width/8+x/8]&(128>>(x&7))))pixel(24+x,80+y);
        in_box(24,641,432,38,gallery.title,24);
        in_box(24,689,432,28,gallery.artist,20);
        in_box(24,724,432,26,gallery.date+" · The Met",20);
    }else{
        round_box(157,229,166,192,3,2);fill(179,373,122,1);fill(179,395,82,1);
        for(int y=0;y<74;++y)for(int x=0;x<118;++x)if(y==std::abs(x-40)/2||y==std::abs(x-86)/2+20)pixel(181+x,270+y);
        in_box(24,466,432,40,gallery.busy?"正在准备今日画作":"每天，一幅新的风景",24);
        in_box(24,524,432,32,gallery.status,20);
        in_box(24,651,432,30,"直连博物馆 · 自动每日轮换",20);
        in_box(24,695,432,30,"已下载的画作会保留在设备中",20);
    }
    fill(24,759,432,1);
    std::string hint=gallery.busy?"正在换画 · 请稍候":!a.connected?"离线留存 · 长按设置":gallery.cached?"留存画作 · 短按换画":"短按换画 · 长按设置";
    in_box(24,768,432,27,hint,20);
}
std::string brief_value(int value) { return value<0?"--":std::to_string(value); }
std::string brief_date(const std::string& date) {
    if(date.size()!=10||date[4]!='-'||date[7]!='-')return "日期未知";
    for(size_t i=0;i<date.size();++i)if(i!=4&&i!=7&&(date[i]<'0'||date[i]>'9'))return "日期未知";
    std::string out=date;out[4]=out[7]='.';return out;
}
void brief_metric(int x,int w,const std::string& title,int value,const char* unit) {
    in_box(x,550,w,28,title,20);
    std::string number=brief_value(value);int size=AValue;
    int span=width(number,size)+4+width(unit,20);
    if(span>w){size=32;span=width(number,size)+4+width(unit,20);}
    number_unit(x+(w-span)/2,610,span,number,unit,size,false,20);
}
void draw_garmin(const AppData& a,time_t now) {
    auto g=paperdesk_garmin::snapshot();
    label(32,34,"GARMIN",20);
    right(448,34,g.ready?brief_date(g.date):timestamp(now,"%Y.%m.%d"),20,220);
    label(32,77,"身体与运动",32,280);
    bool old_day=g.ready&&g.date!=timestamp(now,"%Y-%m-%d");
    bool cached=g.cached||!a.connected||old_day;
    if(g.busy)right(448,88,"更新中",20,120);
    else if(cached&&g.ready)right(448,88,"留存数据",20,120);
    fill(32,123,416,1);
    if(!g.ready){
        // The empty state has no fabricated metrics or progress bars.
        round_box(187,231,102,56,8,2);fill(289,249,7,20);
        fill(198,243,16,32);fill(220,243,16,32);fill(242,243,16,32);
        in_box(32,341,416,50,g.busy?"正在准备身体简报":g.configured?"暂未取得身体数据":"连接你的佳明",32);
        centered_lines(32,424,416,70,g.configured?"读取已同步到佳明云端的数据": "首次在电脑完成账号授权",24);
        centered_lines(32,534,416,70,g.status.empty()?"授权后由设备直接联网更新":g.status,20);
        fill(32,754,416,1);in_box(32,768,416,27,"短按同步 · 长按设置",20);return;
    }

    label(32,151,"身体电量",24,236);
    label(296,153,old_day?"睡眠记录":"昨夜睡眠",20,152);
    in_box(32,196,232,112,brief_value(g.body_battery),WeatherSerif,true);
    if(g.body_battery>=0)progress(32,327,224,8,g.body_battery);
    else {fill(32,331,82,1);fill(174,331,82,1);}
    fill(276,150,1,188);
    std::string sleep="--:--";
    if(g.sleep_seconds>=0){char s[24];snprintf(s,sizeof s,"%d:%02d",g.sleep_seconds/3600,(g.sleep_seconds/60)%60);sleep=s;}
    int sleep_font=width(sleep,AValue)<=152?AValue:32;
    in_box(296,210,152,66,sleep,sleep_font);
    in_box(296,283,152,26,"小时 : 分钟",20);
    in_box(296,321,152,26,"评分 "+brief_value(g.sleep_score),20);
    fill(32,371,416,1);

    label(32,396,old_day?"当日步数":"今日步数",24,180);
    right(448,400,g.step_goal>0?"目标 "+std::to_string(g.step_goal):"目标 --",20,220);
    std::string steps=brief_value(g.steps);
    int step_font=width(steps,ALarge)<=416?ALarge:width(steps,AValue)<=416?AValue:32;
    in_box(32,442,416,69,steps,step_font,true);
    if(g.steps>=0&&g.step_goal>0){
        int progress_value=int(std::clamp(100.0*g.steps/g.step_goal,0.0,100.0));
        progress(32,527,416,6,progress_value);
    }else fill(32,530,416,1);

    brief_metric(32,132,"静息心率",g.resting_hr,"bpm");
    brief_metric(174,132,"平均压力",g.stress,"/100");
    brief_metric(316,132,"夜间 HRV",g.hrv,"ms");
    fill(32,659,416,1);

    bool run=g.activity_type=="running"||g.activity_type=="trail_running"||g.activity_type=="treadmill_running"||g.activity_type=="track_running";
    std::string activity=run?"最近跑步":g.activity_type=="cycling"||g.activity_type=="indoor_cycling"?"最近骑行":g.activity_type=="lap_swimming"||g.activity_type=="open_water_swimming"?"最近游泳":"最近运动";
    label(32,669,activity,20,200);
    if(!g.activity_date.empty())right(448,669,brief_date(g.activity_date.substr(0,10)),20,220);
    if(g.activity_distance_m>=0&&g.activity_distance_m<1e9&&std::isfinite(g.activity_distance_m)){
        char distance[32];double km=g.activity_distance_m/1000.0;
        snprintf(distance,sizeof distance,km>=100?"%.1f":"%.2f",km);
        number_unit(32,725,222,distance,"km",AValue,false,20);
    }else number_unit(32,725,222,"--","km",AValue,false,20);
    std::string duration="用时 --";
    if(g.activity_duration_s>=0&&std::isfinite(g.activity_duration_s)&&g.activity_duration_s<36000000){
        long seconds=std::lround(g.activity_duration_s);char value[40];
        if(seconds>=3600)snprintf(value,sizeof value,"用时 %ld:%02ld:%02ld",seconds/3600,(seconds/60)%60,seconds%60);
        else snprintf(value,sizeof value,"用时 %ld:%02ld",seconds/60,seconds%60);
        duration=value;
    }
    right(448,708,duration,20,180);
    std::string secondary="已同步的运动";
    if(run&&g.activity_duration_s>0&&g.activity_distance_m>0&&std::isfinite(g.activity_duration_s)&&std::isfinite(g.activity_distance_m)){
        double seconds_km=g.activity_duration_s*1000/g.activity_distance_m;
        if(seconds_km<6000){int pace=int(std::lround(seconds_km));char value[40];snprintf(value,sizeof value,"配速 %d'%02d\"/km",pace/60,pace%60);secondary=value;}
        else secondary="配速 --";
    }else if(g.activity_distance_m<0&&g.activity_duration_s<0)secondary="暂无运动记录";
    right(448,737,secondary,20,205);
    fill(32,762,416,1);
    std::string state_hint;
    if(!a.connected)state_hint="离线";
    else if(g.status.find("授权")!=std::string::npos)state_hint="需重新授权";
    else if(g.status.find("受限")!=std::string::npos)state_hint="请求受限";
    else if(g.status.find("失败")!=std::string::npos)state_hint="稍后重试";
    else if(g.busy)state_hint="更新中";
    else if(cached||g.status.find("部分")!=std::string::npos)state_hint="留存数据";
    else state_hint="短按同步";
    std::string sync;
    if(g.last_sync>=1704067200)sync="云端 "+timestamp(g.last_sync,"%m/%d %H:%M");
    else sync="读取 "+timestamp(g.fetched_at,"%H:%M")+" · 云端时间未知";
    std::string suffix=" · "+state_hint;
    // Preserve the actionable state instead of ellipsizing it away when the
    // cloud supplied no sync timestamp; retrieval time is secondary here.
    if(width(sync+suffix,20)>416)sync="云端同步时间未知";
    sync+=suffix;
    in_box(32,770,416,22,sync,20);
}
std::string weight_number(int grams,int unit,bool signed_delta=false){
    if(grams==-1&&!signed_delta)return "--";
    double value=paperdesk_weight::display_value(grams,unit);
    if(std::abs(value)<0.05)value=0;
    char out[32];snprintf(out,sizeof out,signed_delta&&value>0?"+%.1f":"%.1f",value);return out;
}
void draw_weight(const View& view,const AppData& a,time_t now){
    namespace cal=paperdesk_calendar;auto data=paperdesk_garmin::weight_snapshot();
    tm local={};localtime_r(&now,&local);cal::Date today{local.tm_year+1900,local.tm_mon+1,local.tm_mday};
    if(!cal::valid(today)||now<1704067200){label(32,40,"体重月历",32);in_box(32,300,416,50,"请先校准日期",32);return;}
    cal::Date month=cal::add_months(today,view.selection?-1:0);
    const int unit=a.config.weight_unit;const char* unit_name=unit?"斤":"kg";
    label(32,32,"GARMIN",20);right(448,32,timestamp(now,"%Y.%m.%d"),20);label(32,71,"体重月历",32);
    if(data.cached||!a.connected)right(448,83,"留存数据",20,130);
    fill(32,119,416,1);label(32,137,"最近体重",20);right(448,137,"自 "+a.config.weight_start+" 起",20,220);
    number_unit(32,198,205,weight_number(data.latest_grams,unit),unit_name,AValue,false,20);
    bool baseline=data.baseline_date==a.config.weight_start&&data.baseline_grams>=0&&data.latest_grams>=0;
    number_unit(248,198,200,baseline?weight_number(data.latest_grams-data.baseline_grams,unit,true):"--",unit_name,AValue,true,20);
    label(32,238,data.latest_date.empty()?(data.latest_grams<0?"暂无记录":"日期未知"):data.latest_date+" 记录",20,210);
    right(448,238,baseline?"起点 "+weight_number(data.baseline_grams,unit):data.baseline_date!=a.config.weight_start?"起点待同步":"起点无记录",20,190);
    fill(32,267,416,1);
    char title[48];snprintf(title,sizeof title,"%d年%d月",month.year,month.month);in_box(32,276,416,36,title,24);
    label(32,283,view.selection?"上月":"本月",20,90);right(448,283,"短按切换",20,100);
    const char* week[]={"日","一","二","三","四","五","六"};
    for(int col=0;col<7;++col){int x=32+col*416/7,w=(col+1)*416/7-col*416/7;in_box(x,319,w,24,week[col],20);}
    int first=(cal::weekday_monday0(month)+1)%7;int count=cal::days_in_month(month.year,month.month);int rows=(first+count+6)/7;
    constexpr int top=351,height=390;
    for(int day=1;day<=count;++day){
        int index=first+day-1,row=index/7,col=index%7,x=32+col*416/7,w=(col+1)*416/7-col*416/7;
        int y=top+row*height/rows,h=(row+1)*height/rows-row*height/rows;
        bool selected=today.year==month.year&&today.month==month.month&&today.day==day;
        if(selected)round_box(x+2,y,w-4,h-2,6,2);
        char date[40];snprintf(date,sizeof date,"%04d-%02d-%02d",month.year,month.month,day);
        tm prev={};prev.tm_year=month.year-1900;prev.tm_mon=month.month-1;prev.tm_mday=day-1;prev.tm_hour=12;time_t epoch=mktime(&prev);localtime_r(&epoch,&prev);
        char prior[16];strftime(prior,sizeof prior,"%Y-%m-%d",&prev);
        int value=paperdesk_weight::on(data.days,date),before=paperdesk_weight::on(data.days,prior);
        in_box(x,y+2,w,20,std::to_string(day),20);
        in_box(x,y+23,w,19,value<0?"--":weight_number(value,unit),20);
        in_box(x,y+43,w,19,value<0||before<0?"--":weight_number(value-before,unit,true),20);
    }
    fill(32,750,416,1);in_box(32,760,416,28,std::string("单位 ")+(unit?"斤":"kg")+" · 变化较前一日",20);
}
} // namespace

void draw_screen(const View& view,bool full) {
    AppData a;{std::lock_guard<std::mutex> lock(app_mutex);a=app;}
    memset(frame,255,sizeof(frame));
#ifdef PAPERDESK_HOST
    time_t now=preview_now;
#else
    time_t now=time(nullptr);
#endif
    bool clock_ok=now>=1704067200;
    cJSON* snapshot=cJSON_Parse(a.snapshot.c_str());
    auto weather=cJSON_GetObjectItemCaseSensitive(snapshot,"weather");auto codex=cJSON_GetObjectItemCaseSensitive(snapshot,"codex");
    auto windows=cJSON_GetObjectItemCaseSensitive(codex,"windows");
    bool weather_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather,"ok"));
    bool quota_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(codex,"ok"))&&cJSON_GetArraySize(windows)>0;
    bool stale=!a.connected||!a.gateway_error.empty()||(clock_ok&&a.gateway_updated>0&&now-a.gateway_updated>900);
    bool weather_stale=stale||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather,"stale"));
    bool quota_stale=stale||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(codex,"stale"));
    const char* pages[]={"桌面总览","天气","月历","画廊","佳明简报","体重月历"};
    if(view.page==0){
        draw_home(a,weather,codex,windows,weather_ok,quota_ok,weather_stale,quota_stale,now);
    }else if(view.page==1){
        draw_weather(a,weather,weather_ok,weather_stale,now);
    }else if(view.page==2){
        draw_calendar(view,now);
    }else if(view.page==GalleryPage){
        draw_gallery(a);
    }else if(view.page==GarminPage){
        draw_garmin(a,now);
    }else if(view.page==WeightPage){
        draw_weight(view,a,now);
    }else{
        label(32,36,view.details?"网络与配网":"设置",32);
        if(view.details){
            label(32,131,a.connected?"已连接 Wi-Fi":"等待连接 Wi-Fi",24);label(32,182,"http://"+a.ip,24);
            label(32,253,"网页管理码",20);label(32,290,a.config.admin_token,32);
            label(32,378,"配网热点",20);label(32,415,a.ap_name,24);
            label(32,470,"热点密码："+a.ap_password,20);label(32,520,"热点设置：192.168.4.1",20);
            multiline(32,595,a.gateway_error.empty()?"电脑网关已连接":a.gateway_error,20,2);
            label(32,745,"短按返回设置 · 长按回首页",20);
        }else{
            const char* labels[]={"网络与手机设置","首页风格","天气风格","开机默认页面","自动轮播","夜间暂停轮播","体重单位","立即同步数据"};
            std::string values[]={"查看",a.config.home_style==1?"编辑式微衬线":"精密桌面钟",a.config.weather_style==1?"温差观测":"天气日报",pages[std::clamp(a.config.default_page,0,ContentPageCount-1)],a.config.rotation_minutes?"5分钟":"关闭",a.config.night?"开启":"关闭",a.config.weight_unit?"斤":"公斤","请求"};
            for(int i=0;i<8;i++){int y=125+i*54;if(view.selection==i)round_box(24,y-10,432,49,8,2);label(40,y,labels[i],24,270);right(432,y+3,values[i],20,160);}
            label(32,580,"管理码："+a.config.admin_token,24);label(32,628,"http://"+a.ip,20);label(32,680,"纸间 Paperdesk 0.8.0",20);label(32,751,"拨动选择 · 短按修改 · 长按回首页",20);
        }
    }
    cJSON_Delete(snapshot);epaper_timed_out=false;
    if(full){EPD_Init();EPD_Display_Base(frame);}else EPD_Display_Partial(frame,0,0,800,480);
    if(epaper_timed_out)ESP_LOGE("screen","BUSY timeout; check power and wiring");
    else ESP_LOGI("screen","page=%d mode=%d drawn",view.page,view.mode);
}
