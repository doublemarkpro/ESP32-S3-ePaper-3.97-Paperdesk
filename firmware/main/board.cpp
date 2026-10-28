#include "paperdesk.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sys/time.h>
#include <cmath>

static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t rtc,sht;
static int bcd(uint8_t b){return (b>>4)*10+(b&15);}
static uint8_t to_bcd(int n){return (n/10)*16+n%10;}
static uint8_t crc8(const uint8_t *p){uint8_t c=255;for(int i=0;i<2;i++){c^=p[i];for(int j=0;j<8;j++)c=c&128?(c<<1)^0x31:c<<1;}return c;}
static void key_task(void*){
    const gpio_num_t pins[]={GPIO_NUM_4,GPIO_NUM_5,GPIO_NUM_6};
    int raw[3]={1,1,1},stable[3]={1,1,1};int64_t change[3]={},start[3]={};bool held[3]={};
    for(;;){int64_t now=esp_timer_get_time()/1000;for(int i=0;i<3;i++){
        int v=gpio_get_level(pins[i]);if(v!=raw[i]){raw[i]=v;change[i]=now;}
        if(now-change[i]>=30 && stable[i]!=v){stable[i]=v;if(!v){start[i]=now;held[i]=false;if(i!=1){int key=i==0?Up:Down;xQueueSend(button_queue,&key,0);}}
            else if(i==1&&!held[i]){int key=Press;xQueueSend(button_queue,&key,0);}}
        if(i==1&&!stable[i]&&!held[i]&&now-start[i]>=850){held[i]=true;int key=Back;xQueueSend(button_queue,&key,0);}
    }vTaskDelay(pdMS_TO_TICKS(10));}
}
void board_init(){
    i2c_master_bus_config_t bc={};bc.i2c_port=I2C_NUM_0;bc.sda_io_num=GPIO_NUM_41;bc.scl_io_num=GPIO_NUM_42;bc.clk_source=I2C_CLK_SRC_DEFAULT;bc.glitch_ignore_cnt=7;bc.flags.enable_internal_pullup=true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc,&bus));
    i2c_device_config_t dc={};dc.dev_addr_length=I2C_ADDR_BIT_LEN_7;dc.scl_speed_hz=100000;dc.device_address=0x51;ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&dc,&rtc));dc.device_address=0x70;ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&dc,&sht));
    uint8_t reg=4,buf[7];
    if(i2c_master_transmit_receive(rtc,&reg,1,buf,7,100)==ESP_OK && !(buf[0]&128)){
        tm t={};t.tm_sec=bcd(buf[0]&127);t.tm_min=bcd(buf[1]&127);t.tm_hour=bcd(buf[2]&63);t.tm_mday=bcd(buf[3]&63);t.tm_mon=bcd(buf[5]&31)-1;t.tm_year=bcd(buf[6])+100;t.tm_isdst=-1;
        time_t epoch=mktime(&t);if(t.tm_year>=124 && t.tm_mon>=0&&t.tm_mon<12&&t.tm_mday>0&&t.tm_mday<=31){timeval tv={epoch,0};settimeofday(&tv,nullptr);app.rtc_ok=true;}
    }
    gpio_config_t gc={};gc.pin_bit_mask=(1ULL<<4)|(1ULL<<5)|(1ULL<<6);gc.mode=GPIO_MODE_INPUT;gc.pull_up_en=GPIO_PULLUP_ENABLE;ESP_ERROR_CHECK(gpio_config(&gc));
    xTaskCreate(key_task,"rocker",3072,nullptr,4,nullptr);
}
void rtc_store_system_time(){time_t now=time(nullptr);if(now<1704067200)return;tm t;localtime_r(&now,&t);uint8_t p[]={4,to_bcd(t.tm_sec),to_bcd(t.tm_min),to_bcd(t.tm_hour),to_bcd(t.tm_mday),uint8_t(t.tm_wday),to_bcd(t.tm_mon+1),to_bcd(t.tm_year-100)};bool ok=i2c_master_transmit(rtc,p,sizeof(p),100)==ESP_OK;std::lock_guard<std::mutex> lock(app_mutex);app.rtc_ok=ok;}
void sample_environment(){
    const uint8_t wake[]={0x35,0x17},cmd[]={0x78,0x66},sleep[]={0xb0,0x98};
    uint8_t p[6]={};bool ok=false;esp_err_t status=ESP_OK;
    for(int attempt=0;attempt<3&&!ok;attempt++){
        status=i2c_master_transmit(sht,wake,2,100);
        // Round up: pdMS_TO_TICKS(2) alone is zero with the 100 Hz tick.
        vTaskDelay(pdMS_TO_TICKS(2)+1);
        if(status==ESP_OK)status=i2c_master_transmit(sht,cmd,2,100);
        vTaskDelay(pdMS_TO_TICKS(20)+1);
        if(status==ESP_OK)status=i2c_master_receive(sht,p,6,100);
        ok=status==ESP_OK&&crc8(p)==p[2]&&crc8(p+3)==p[5];
        i2c_master_transmit(sht,sleep,2,100);
        if(!ok)vTaskDelay(pdMS_TO_TICKS(10)+1);
    }
    std::lock_guard<std::mutex> lock(app_mutex);app.env_ok=ok;
    if(ok){app.temperature=-45+175.0f*((p[0]<<8)|p[1])/65536;app.humidity=100.0f*((p[3]<<8)|p[4])/65536;ESP_LOGI("board","SHTC3 temperature=%.1f C humidity=%.1f %%",app.temperature,app.humidity);}
    else ESP_LOGW("board","SHTC3 read failed: %s (CRC valid=%d)",esp_err_to_name(status),crc8(p)==p[2]&&crc8(p+3)==p[5]);
}
