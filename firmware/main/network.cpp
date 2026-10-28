#include "paperdesk.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <sys/time.h>
#include <cstring>
#include <algorithm>

extern const unsigned char web_start[] asm("_binary_web_html_start");
extern const unsigned char web_end[] asm("_binary_web_html_end");
static httpd_handle_t server;
static nvs_handle_t storage;
static std::atomic<int64_t> reboot_at{0};
static std::string config_json(){auto j=cJSON_CreateObject();cJSON_AddStringToObject(j,"ssid",app.config.ssid.c_str());cJSON_AddStringToObject(j,"wifi_password",app.config.wifi_password.c_str());cJSON_AddStringToObject(j,"gateway",app.config.gateway.c_str());cJSON_AddStringToObject(j,"gateway_token",app.config.gateway_token.c_str());cJSON_AddStringToObject(j,"admin_token",app.config.admin_token.c_str());cJSON_AddNumberToObject(j,"layout_version",2);cJSON_AddNumberToObject(j,"home_style",app.config.home_style);cJSON_AddNumberToObject(j,"weather_style",app.config.weather_style);cJSON_AddNumberToObject(j,"default_page",app.config.default_page);cJSON_AddNumberToObject(j,"rotation_minutes",app.config.rotation_minutes);cJSON_AddBoolToObject(j,"night",app.config.night);char*p=cJSON_PrintUnformatted(j);std::string out=p?p:"{}";cJSON_free(p);cJSON_Delete(j);return out;}
void config_save(){std::string j=config_json();ESP_ERROR_CHECK(nvs_set_str(storage,"config",j.c_str()));ESP_ERROR_CHECK(nvs_commit(storage));}
void config_load(){
    esp_err_t e=nvs_flash_init_partition("storage");if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase_partition("storage"));e=nvs_flash_init_partition("storage");}ESP_ERROR_CHECK(e);ESP_ERROR_CHECK(nvs_open_from_partition("storage","paperdesk",NVS_READWRITE,&storage));
    size_t n=0;if(nvs_get_str(storage,"config",nullptr,&n)==ESP_OK&&n<8192){std::string s(n,'\0');nvs_get_str(storage,"config",s.data(),&n);auto j=cJSON_Parse(s.c_str());app.config.ssid=json_string(j,"ssid");app.config.wifi_password=json_string(j,"wifi_password");app.config.gateway=json_string(j,"gateway");app.config.gateway_token=json_string(j,"gateway_token");app.config.admin_token=json_string(j,"admin_token");int saved_page=json_int(j,"default_page");app.config.default_page=json_int(j,"layout_version")<2?(saved_page==3?2:saved_page==1?1:0):std::clamp(saved_page,0,2);app.config.rotation_minutes=json_int(j,"rotation_minutes")==5?5:0;app.config.night=!cJSON_IsFalse(cJSON_GetObjectItem(j,"night"));auto style=cJSON_GetObjectItemCaseSensitive(j,"home_style");app.config.home_style=cJSON_IsNumber(style)&&style->valuedouble==1?1:0;style=cJSON_GetObjectItemCaseSensitive(j,"weather_style");app.config.weather_style=cJSON_IsNumber(style)&&style->valuedouble==1?1:0;cJSON_Delete(j);}
    if(app.config.admin_token.empty()){char token[12];snprintf(token,sizeof token,"%08lx",(unsigned long)esp_random());app.config.admin_token=token;config_save();}
    size_t size=0;if(nvs_get_blob(storage,"snapshot",nullptr,&size)==ESP_OK&&size<32768){std::string s(size,'\0');if(nvs_get_blob(storage,"snapshot",s.data(),&size)==ESP_OK){app.snapshot=s;auto j=cJSON_Parse(s.c_str());app.gateway_updated=json_int(j,"generated_at");cJSON_Delete(j);}}
}
static bool auth(httpd_req_t*r){size_t n=httpd_req_get_hdr_value_len(r,"X-Paperdesk-Token");char value[100]={};if(n<sizeof value)httpd_req_get_hdr_value_str(r,"X-Paperdesk-Token",value,sizeof value);std::lock_guard<std::mutex> guard(app_mutex);return !app.config.admin_token.empty()&&app.config.admin_token==value;}
static esp_err_t json_reply(httpd_req_t*r,const std::string&s){httpd_resp_set_type(r,"application/json; charset=utf-8");httpd_resp_set_hdr(r,"Cache-Control","no-store");return httpd_resp_send(r,s.c_str(),s.size());}
static esp_err_t denied(httpd_req_t*r){httpd_resp_set_status(r,"401 Unauthorized");return json_reply(r,"{\"error\":\"管理码不正确，请查看设备设置页\"}");}
static esp_err_t page_handler(httpd_req_t*r){httpd_resp_set_type(r,"text/html; charset=utf-8");httpd_resp_set_hdr(r,"Cache-Control","no-store");httpd_resp_set_hdr(r,"Content-Security-Policy","default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");return httpd_resp_send(r,(const char*)web_start,web_end-web_start);}
static esp_err_t state_handler(httpd_req_t*r){auto j=cJSON_CreateObject();{std::lock_guard<std::mutex> guard(app_mutex);cJSON_AddStringToObject(j,"ip",app.ip.c_str());cJSON_AddBoolToObject(j,"connected",app.connected);cJSON_AddBoolToObject(j,"sensor_ok",app.env_ok);if(app.env_ok){cJSON_AddNumberToObject(j,"temperature",app.temperature);cJSON_AddNumberToObject(j,"humidity",app.humidity);}cJSON_AddStringToObject(j,"gateway_status",app.gateway_error.empty()?"已同步":app.gateway_error.c_str());cJSON_AddStringToObject(j,"version","0.5.0");cJSON_AddNumberToObject(j,"home_style",app.config.home_style);cJSON_AddNumberToObject(j,"weather_style",app.config.weather_style);}char*p=cJSON_PrintUnformatted(j);std::string s=p;cJSON_free(p);cJSON_Delete(j);return json_reply(r,s);}
static esp_err_t config_handler(httpd_req_t*r){if(!auth(r))return denied(r);if(r->method==HTTP_GET){auto j=cJSON_CreateObject();{std::lock_guard<std::mutex> guard(app_mutex);auto&c=app.config;cJSON_AddStringToObject(j,"ssid",c.ssid.c_str());cJSON_AddStringToObject(j,"gateway",c.gateway.c_str());cJSON_AddBoolToObject(j,"wifi_password_set",!c.wifi_password.empty());cJSON_AddBoolToObject(j,"gateway_token_set",!c.gateway_token.empty());cJSON_AddNumberToObject(j,"home_style",c.home_style);cJSON_AddNumberToObject(j,"weather_style",c.weather_style);cJSON_AddNumberToObject(j,"default_page",c.default_page);cJSON_AddNumberToObject(j,"rotation_minutes",c.rotation_minutes);cJSON_AddBoolToObject(j,"night",c.night);}char*p=cJSON_PrintUnformatted(j);std::string s=p;cJSON_free(p);cJSON_Delete(j);return json_reply(r,s);}
    if(r->content_len<=0||r->content_len>4096){httpd_resp_set_status(r,"400 Bad Request");return json_reply(r,"{\"error\":\"请求过大\"}");}std::string body(r->content_len,'\0');int total=0;while(total<r->content_len){int n=httpd_req_recv(r,body.data()+total,r->content_len-total);if(n<=0)return ESP_FAIL;total+=n;}auto j=cJSON_Parse(body.c_str());if(!cJSON_IsObject(j)){cJSON_Delete(j);httpd_resp_set_status(r,"400 Bad Request");return json_reply(r,"{\"error\":\"无效JSON\"}");}
    Config next;{std::lock_guard<std::mutex> guard(app_mutex);next=app.config;}
    bool bad=false;for(const char*key:{"ssid","wifi_password","gateway","gateway_token"}){auto v=cJSON_GetObjectItemCaseSensitive(j,key);if(!v)continue;if(!cJSON_IsString(v)){bad=true;break;}std::string s=v->valuestring;size_t limit=strcmp(key,"ssid")==0?32:strcmp(key,"wifi_password")==0?63:255;if(s.size()>limit||s.find_first_of("\r\n")!=std::string::npos){bad=true;break;}if(!strcmp(key,"ssid"))next.ssid=s;if(!strcmp(key,"wifi_password")&&!s.empty())next.wifi_password=s;if(!strcmp(key,"gateway")){while(!s.empty()&&s.back()=='/')s.pop_back();next.gateway=s;}if(!strcmp(key,"gateway_token")&&!s.empty())next.gateway_token=s;}
    if(!next.gateway.empty()&&(next.gateway.rfind("http://",0)!=0&&next.gateway.rfind("https://",0)!=0))bad=true;
    if(next.gateway.find_first_of("@?#")!=std::string::npos)bad=true;
    // Both style selectors accept only JSON numbers exactly equal to 0 or 1.
    // cJSON valueint alone would silently accept fractions such as 1.5.
    for(const char* key:{"home_style","weather_style"}){
        auto style=cJSON_GetObjectItemCaseSensitive(j,key);
        if(!style)continue;
        if(!cJSON_IsNumber(style)||(style->valuedouble!=0&&style->valuedouble!=1))bad=true;
        else if(!strcmp(key,"home_style"))next.home_style=static_cast<int>(style->valuedouble);
        else next.weather_style=static_cast<int>(style->valuedouble);
    }
    auto pg=cJSON_GetObjectItem(j,"default_page");if(pg){if(!cJSON_IsNumber(pg)||pg->valueint<0||pg->valueint>2)bad=true;else next.default_page=pg->valueint;}
    auto rot=cJSON_GetObjectItem(j,"rotation_minutes");if(rot){if(!cJSON_IsNumber(rot)||(rot->valueint!=0&&rot->valueint!=5))bad=true;else next.rotation_minutes=rot->valueint;}
    auto night=cJSON_GetObjectItem(j,"night");if(night){if(!cJSON_IsBool(night))bad=true;else next.night=cJSON_IsTrue(night);}
    auto epoch=cJSON_GetObjectItem(j,"epoch");time_t set_epoch=cJSON_IsNumber(epoch)?(time_t)epoch->valuedouble:0;cJSON_Delete(j);
    if(bad){httpd_resp_set_status(r,"400 Bad Request");return json_reply(r,"{\"error\":\"请检查字段长度、网关地址或配置值\"}");}
    bool restart,style_changed,weather_style_changed;{std::lock_guard<std::mutex> guard(app_mutex);restart=next.ssid!=app.config.ssid||next.wifi_password!=app.config.wifi_password;style_changed=next.home_style!=app.config.home_style;weather_style_changed=next.weather_style!=app.config.weather_style;app.config=next;config_save();}
    if(set_epoch>=1704067200&&set_epoch<4102444800){timeval tv={set_epoch,0};settimeofday(&tv,nullptr);rtc_store_system_time();}
    if(style_changed)full_refresh=true;
    if(weather_style_changed)weather_full_refresh=true;
    force_sync=true;dirty=true;if(restart)reboot_at=esp_timer_get_time()+3000000;
    return json_reply(r,restart?"{\"ok\":true,\"restart\":true}":"{\"ok\":true,\"restart\":false}");
}
static void on_wifi(void*,esp_event_base_t base,int32_t id,void *data){if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_START)esp_wifi_connect();if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){{std::lock_guard<std::mutex> lock(app_mutex);app.connected=false;app.ip="192.168.4.1";}esp_wifi_connect();dirty=true;}if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){auto e=(ip_event_got_ip_t*)data;char ip[20];snprintf(ip,sizeof ip,IPSTR,IP2STR(&e->ip_info.ip));{std::lock_guard<std::mutex> lock(app_mutex);app.connected=true;app.ip=ip;}dirty=true;force_sync=true;}}
static esp_err_t http_event(esp_http_client_event_t*e){if(e->event_id==HTTP_EVENT_ON_DATA){auto s=(std::string*)e->user_data;if(s->size()+e->data_len>32768)return ESP_ERR_NO_MEM;s->append((const char*)e->data,e->data_len);}return ESP_OK;}
static void fetch_snapshot(){Config c;bool connected;{std::lock_guard<std::mutex> lock(app_mutex);c=app.config;connected=app.connected;}if(!connected||c.gateway.empty())return;std::string body,url=c.gateway+"/v1/snapshot";esp_http_client_config_t hc={};hc.url=url.c_str();hc.event_handler=http_event;hc.user_data=&body;hc.timeout_ms=10000;hc.crt_bundle_attach=esp_crt_bundle_attach;hc.buffer_size=2048;hc.disable_auto_redirect=true;auto client=esp_http_client_init(&hc);if(!client)return;std::string bearer="Bearer "+c.gateway_token;esp_http_client_set_header(client,"Authorization",bearer.c_str());esp_http_client_set_header(client,"Accept-Encoding","identity");esp_err_t result=esp_http_client_perform(client);int status=esp_http_client_get_status_code(client);esp_http_client_cleanup(client);
    cJSON*j=result==ESP_OK&&status==200?cJSON_Parse(body.c_str()):nullptr;bool valid=cJSON_IsObject(j)&&json_int(j,"schema")==1;{std::lock_guard<std::mutex> lock(app_mutex);if(valid){app.snapshot=body;app.gateway_updated=json_int(j,"generated_at");app.gateway_error.clear();static int64_t last_save=0;int64_t now=esp_timer_get_time();if(!last_save||now-last_save>1800000000LL){nvs_set_blob(storage,"snapshot",body.data(),body.size());nvs_commit(storage);last_save=now;}}else{app.gateway_error=status==401?"网关访问令牌不正确":status==200?"网关数据格式无效":"网关连接失败 · 保留缓存";}}cJSON_Delete(j);dirty=true;
}
static void network_task(void*){uint32_t seconds=0;for(;;){int64_t restart_at=reboot_at.load();if(restart_at&&esp_timer_get_time()>restart_at)esp_restart();if(force_sync.exchange(false)||seconds%300==0)fetch_snapshot();if(seconds%300==0&&time(nullptr)>=1704067200)rtc_store_system_time();vTaskDelay(pdMS_TO_TICKS(1000));seconds++;}}
void network_start(){
    ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());esp_netif_create_default_wifi_sta();esp_netif_create_default_wifi_ap();wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,on_wifi,nullptr));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,on_wifi,nullptr));
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_WIFI_SOFTAP);char name[32],password[24];snprintf(name,sizeof name,"Paperdesk-%02X%02X",mac[4],mac[5]);snprintf(password,sizeof password,"paper%02x%02x",mac[4],mac[5]);
    app.ap_name=name;app.ap_password=password;app.ap_active=true;wifi_config_t ap={};strncpy((char*)ap.ap.ssid,name,sizeof ap.ap.ssid-1);strncpy((char*)ap.ap.password,password,sizeof ap.ap.password-1);ap.ap.ssid_len=strlen(name);ap.ap.channel=6;ap.ap.max_connection=3;ap.ap.authmode=WIFI_AUTH_WPA2_PSK;ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    if(!app.config.ssid.empty()){wifi_config_t sta={};memcpy(sta.sta.ssid,app.config.ssid.data(),app.config.ssid.size());memcpy(sta.sta.password,app.config.wifi_password.data(),app.config.wifi_password.size());ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));}
    ESP_ERROR_CHECK(esp_wifi_start());esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);esp_sntp_setservername(0,"ntp.aliyun.com");esp_sntp_init();
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();config.stack_size=8192;config.max_uri_handlers=8;ESP_ERROR_CHECK(httpd_start(&server,&config));
    const httpd_uri_t uris[]={{.uri="/",.method=HTTP_GET,.handler=page_handler,.user_ctx=nullptr},{.uri="/api/state",.method=HTTP_GET,.handler=state_handler,.user_ctx=nullptr},{.uri="/api/config",.method=HTTP_GET,.handler=config_handler,.user_ctx=nullptr},{.uri="/api/config",.method=HTTP_POST,.handler=config_handler,.user_ctx=nullptr}};for(auto &u:uris)ESP_ERROR_CHECK(httpd_register_uri_handler(server,&u));
    xTaskCreate(network_task,"network",12288,nullptr,3,nullptr);
}
