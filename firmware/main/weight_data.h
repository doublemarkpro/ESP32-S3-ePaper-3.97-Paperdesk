#pragma once
#include "garmin.h"
#include "garmin_data.h"
#include <algorithm>
#include <cmath>

namespace paperdesk_weight {
using paperdesk_garmin::WeightDay;
inline int grams(const cJSON* value) {
    if(!cJSON_IsNumber(value)||!std::isfinite(value->valuedouble)||value->valuedouble<10000||value->valuedouble>500000)return -1;
    return int(std::lround(value->valuedouble));
}
// Garmin returns grams and may return multiple unsorted samples on a day.
inline bool parse_range(const cJSON* root,std::vector<WeightDay>& days) {
    const cJSON* rows=garmin_data::field(root,"dateWeightList");
    if(!cJSON_IsArray(rows))return false;
    std::vector<WeightDay> result;
    const cJSON* row=nullptr;
    cJSON_ArrayForEach(row,rows){
        auto date=garmin_data::field(row,"calendarDate");
        int value=grams(garmin_data::field(row,"weight"));
        time_t timestamp=garmin_data::cloud_time(garmin_data::field(row,"timestampGMT"));
        if(!cJSON_IsString(date)||!garmin_data::date_valid(date->valuestring)||value<0||!timestamp)continue;
        auto it=std::find_if(result.begin(),result.end(),[&](const WeightDay& d){return d.date==date->valuestring;});
        if(it==result.end()){
            if(result.size()>=64)return false;
            result.push_back({date->valuestring,value,timestamp});
        }else if(timestamp>it->timestamp){it->grams=value;it->timestamp=timestamp;}
    }
    std::sort(result.begin(),result.end(),[](const WeightDay&a,const WeightDay&b){return a.date<b.date;});
    days=std::move(result);return true;
}
inline int on(const std::vector<WeightDay>& days,const std::string& date){
    for(const auto& d:days)if(d.date==date)return d.grams;
    return -1;
}
inline double display_value(int grams,int unit){return grams*(unit==1?2.0:1.0)/1000.0;}
}
