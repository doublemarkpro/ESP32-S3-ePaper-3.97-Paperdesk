#include "weight_data.h"
#include <cassert>
#include <cmath>

int main(){
    const char* sample=R"({"dateWeightList":[
      {"calendarDate":"2026-10-03","weight":72500,"timestampGMT":1791000000000},
      {"calendarDate":"2026-10-01","weight":73000,"timestampGMT":1790900000000},
      {"calendarDate":"2026-10-03","weight":72400,"timestampGMT":1791000200000},
      {"calendarDate":"2026-10-03","weight":72900,"timestampGMT":1791000100000},
      {"calendarDate":"2026-10-04","weight":null,"timestampGMT":1791000000000},
      {"calendarDate":"2026-02-30","weight":72000,"timestampGMT":1791000000000}]})";
    cJSON* j=cJSON_Parse(sample);assert(j);std::vector<paperdesk_garmin::WeightDay> days;
    assert(paperdesk_weight::parse_range(j,days));cJSON_Delete(j);
    assert(days.size()==2&&days[0].date=="2026-10-01"&&days[1].date=="2026-10-03");
    assert(paperdesk_weight::on(days,"2026-10-03")==72400);
    assert(paperdesk_weight::on(days,"2026-10-02")==-1);
    assert(paperdesk_weight::on(days,"2026-10-04")==-1);
    assert(std::abs(paperdesk_weight::display_value(72400,0)-72.4)<0.00001);
    assert(std::abs(paperdesk_weight::display_value(72400,1)-144.8)<0.00001);
    assert(std::abs(paperdesk_weight::display_value(-200,1)+0.4)<0.00001);
    j=cJSON_Parse(R"({"dateWeightList":[]})");assert(paperdesk_weight::parse_range(j,days)&&days.empty());cJSON_Delete(j);
    j=cJSON_Parse(R"({"dateWeightList":null})");assert(!paperdesk_weight::parse_range(j,days));cJSON_Delete(j);
    j=cJSON_Parse("-1");assert(paperdesk_weight::grams(j)==-1);cJSON_Delete(j);
}
