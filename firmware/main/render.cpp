#include "paperdesk.h"
#include "epaper_port.h"
#include "weather_icons.h"
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
namespace {
constexpr int AClock=201, ALarge=202, AValue=203, BClock=204, BLarge=205, BValue=206;
uint8_t frame[48000];
uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p+2)) << 16); }
const uint8_t* font(int size) {
    switch(size) { case AClock:return theme_a_clock; case ALarge:return theme_a_large; case AValue:return theme_a_value; case BClock:return theme_b_clock; case BLarge:return theme_b_large; case BValue:return theme_b_value; case 20:return f20; case 32:return f32; case 56:return f56; case 112:return f112; case 144:return f144; default:return f24; }
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
void raw_text(int x,int y,const std::string& s,int size) {
    const uint8_t* f=font(size);int fs=u16(f),count=u16(f+2),stride=(fs+7)/8;const char* p=s.c_str();
    while(*p){auto g=glyph(f,decode(p));
        if(g){const uint8_t* bits=f+4+count*8+((g-f-4)/8)*fs*stride;
            for(int yy=0;yy<fs;yy++)for(int xx=0;xx<fs;xx++)if(bits[yy*stride+xx/8]&(128>>(xx&7)))pixel(x+xx,y+yy);
            x+=u16(g+4);
        }else{round_box(x,y,size-2,size-2,2,2);x+=size;}
    }
}
void label(int x,int ink_top,const std::string& value,int size=24,int max_width=416) {
    auto s=fit(value,size,max_width);auto b=measure(s,size);raw_text(x-b.left,ink_top-b.top,s,size);
}
void right(int edge,int ink_top,const std::string& s,int size=20,int max_width=416) {
    auto value=fit(s,size,max_width);auto b=measure(value,size);raw_text(edge-b.right,ink_top-b.top,value,size);
}
void in_box(int x,int y,int w,int h,const std::string& value,int size,bool align_left=false) {
    auto s=fit(value,size,w);auto b=measure(s,size);
    raw_text((align_left?x:x+(w-b.w())/2)-b.left,y+(h-b.h())/2-b.top,s,size);
}
void multiline(int x,int y,const std::string& s,int size=24,int rows=3,int w=416) {
    std::string row;const char* p=s.c_str();
    while(*p&&rows>0){const char* start=p;decode(p);std::string c(start,p-start);if(width(row+c,size)>w){label(x,y,row,size,w);y+=size+14;--rows;row.clear();}row+=c;}
    if(rows>0&&!row.empty())label(x,y,row,size,w);
}
// A shared baseline keeps °C and % attached to their numerals. Use the ink
// bounds of the complete group to center its contents inside the weather row.
void number_unit(int x,int center_y,int area_width,const std::string& number,const std::string& unit,int number_size=56,bool align_right=false) {
    int unit_size=24;
    while(number_size>32&&width(number,number_size)+width(unit,unit_size)+4>area_width)
        number_size=number_size==ALarge?AValue:number_size==BLarge?BValue:32;
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
void detail_footer(const std::string& name,int page,const std::string& hint="拨动切页 · 长按设置") {
    fill(32,720,416,1);label(32,745,hint,20);label(32,770,name,20);
    right(448,770,std::to_string(page+1)+" / 3",20);
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
} // namespace

void draw_screen(const View& view,bool full) {
    AppData a;{std::lock_guard<std::mutex> lock(app_mutex);a=app;}
    memset(frame,255,sizeof(frame));time_t now=time(nullptr);tm local={};localtime_r(&now,&local);
    bool clock_ok=now>=1704067200,night=clock_ok&&(local.tm_hour<6||local.tm_hour>=18);
    cJSON* snapshot=cJSON_Parse(a.snapshot.c_str());
    auto weather=cJSON_GetObjectItemCaseSensitive(snapshot,"weather");auto codex=cJSON_GetObjectItemCaseSensitive(snapshot,"codex");
    auto windows=cJSON_GetObjectItemCaseSensitive(codex,"windows");
    bool weather_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather,"ok"));
    bool quota_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(codex,"ok"))&&cJSON_GetArraySize(windows)>0;
    bool stale=!a.connected||!a.gateway_error.empty()||(clock_ok&&a.gateway_updated>0&&now-a.gateway_updated>900);
    bool weather_stale=stale||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(weather,"stale"));
    bool quota_stale=stale||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(codex,"stale"));
    const char* pages[]={"桌面总览","天气","Codex"};
    if(view.page==0){
        draw_home(a,weather,codex,windows,weather_ok,quota_ok,weather_stale,quota_stale,now);
    }else if(view.page==1){
        label(32,36,"天气",32);right(448,44,clock_ok?timestamp(now,"%H:%M"):"--:--",20);
        if(weather_ok){
            draw_weather_icon(36,117,80,weather_code(weather),json_string(weather,"text"),night,pixel);
            label(144,125,json_string(weather,"city"),32,290);label(144,172,json_string(weather,"text"),24,290);
            number_unit(32,280,240,json_string(weather,"temp","--"),"°C",112);
            label(285,263,"体感 "+std::string(json_string(weather,"feels_like","--"))+"°C",20,163);
            label(32,361,std::string(json_string(weather,"wind"))+" "+json_string(weather,"wind_scale")+"级",20,265);
            right(448,361,"湿度 "+std::string(json_string(weather,"humidity"))+"%",20,145);
            fill(32,408,416,1);label(32,432,"未来三天",24);
            auto days=cJSON_GetObjectItemCaseSensitive(weather,"daily");
            for(int i=0;i<std::min(3,cJSON_GetArraySize(days));i++){
                auto day=cJSON_GetArrayItem(days,i);int cy=499+i*66;
                in_box(32,cy-20,52,40,i==0?"今天":i==1?"明天":"后天",20,true);
                draw_weather_icon(101,cy-24,48,weather_code(day),json_string(day,"text"),false,pixel);
                in_box(164,cy-20,130,40,json_string(day,"text"),20,true);
                right(448,cy-10,std::string(json_string(day,"min"))+" / "+json_string(day,"max")+"°",20,145);
            }
            label(32,680,std::string(weather_stale?"缓存 · ":"")+"和风天气 "+timestamp(json_int(weather,"updated_at"),"%H:%M"),20);
        }else{draw_weather_icon(200,154,80,-1,"",false,pixel);in_box(32,280,416,44,"天气尚未连接",32);multiline(32,366,json_string(weather,"error","请在电脑网关填写和风天气信息"),24,4);label(32,630,"长按拨轮进入设置",20);}
        detail_footer("天气",1,"按下同步 · 长按设置");
    }else if(view.page==2){
        label(32,36,"Codex",32);right(448,44,clock_ok?timestamp(now,"%H:%M"):"--:--",20);
        if(quota_ok){auto bucket=cJSON_GetArrayItem(windows,std::min(view.selection,cJSON_GetArraySize(windows)-1));int remaining=std::clamp(json_int(bucket,"remaining"),0,100);
            label(32,150,quota_label(bucket),24);in_box(32,220,416,140,std::to_string(remaining)+"%",112,true);label(32,390,"剩余额度",24);
            progress(32,449,416,14,remaining);
            label(32,558,"下次重置 · 北京时间",20);label(32,599,timestamp(json_int(bucket,"resets_at")),32);
            label(32,680,std::string(quota_stale?"缓存 · ":"更新于 ")+timestamp(json_int(codex,"updated_at")),20);
        }else{in_box(32,180,416,64,"等待额度数据",32);multiline(32,300,json_string(codex,"error",a.gateway_error.c_str()),24,4);label(32,560,"长按拨轮进入设置",20);}
        detail_footer("Codex",2,"按下切换窗口 · 长按设置");
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
            const char* labels[]={"网络与手机设置","首页风格","开机默认页面","自动轮播","夜间暂停轮播","立即同步数据"};
            std::string values[]={"查看",a.config.home_style==1?"编辑式微衬线":"精密桌面钟",pages[std::clamp(a.config.default_page,0,2)],a.config.rotation_minutes?"5分钟":"关闭",a.config.night?"开启":"关闭","请求"};
            for(int i=0;i<6;i++){int y=145+i*70;if(view.selection==i)round_box(24,y-16,432,64,8,2);label(40,y,labels[i],24,270);right(432,y+3,values[i],20,160);}
            label(32,590,"管理码："+a.config.admin_token,24);label(32,638,"http://"+a.ip,20);label(32,690,"纸间 Paperdesk 0.3.0",20);label(32,751,"拨动选择 · 短按修改 · 长按回首页",20);
        }
    }
    cJSON_Delete(snapshot);epaper_timed_out=false;
    if(full){EPD_Init();EPD_Display_Base(frame);}else EPD_Display_Partial(frame,0,0,800,480);
    if(epaper_timed_out)ESP_LOGE("screen","BUSY timeout; check power and wiring");
    else ESP_LOGI("screen","page=%d mode=%d drawn",view.page,view.mode);
}
