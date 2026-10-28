#pragma once
#include <stdint.h>
extern bool epaper_timed_out;
void EPD_Init();
void EPD_Display_Base(const uint8_t*);
void EPD_Display_Partial(const uint8_t*,int,int,int,int);
