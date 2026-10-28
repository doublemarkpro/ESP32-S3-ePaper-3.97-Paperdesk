#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace weather_ui {
struct Range { bool valid=false; int low=0,high=0; };
struct Axis {
    bool valid=false;
    int low=0,high=0;
    int project(int value,int left,int right) const {
        if(!valid||high<=low)return left;
        return left+int(std::lround(double(std::clamp(value,low,high)-low)*(right-left)/(high-low)));
    }
};
inline bool temperature(const char* text,int& value) {
    if(!text||!*text)return false;
    char* end=nullptr;long parsed=std::strtol(text,&end,10);
    if(end==text)return false;
    while(*end&&std::isspace(static_cast<unsigned char>(*end)))++end;
    if(*end||parsed < -100||parsed > 100)return false;
    value=static_cast<int>(parsed);return true;
}
inline Range range(const char* low,const char* high) {
    Range r;r.valid=temperature(low,r.low)&&temperature(high,r.high)&&r.low<=r.high;return r;
}
inline Axis shared_axis(const std::array<Range,3>& ranges) {
    Axis axis;
    for(auto r:ranges)if(r.valid){
        if(!axis.valid){axis.low=r.low;axis.high=r.high;axis.valid=true;}
        else{axis.low=std::min(axis.low,r.low);axis.high=std::max(axis.high,r.high);}
    }
    if(axis.valid){
        axis.low=int(std::floor(axis.low/5.0))*5;axis.high=int(std::ceil(axis.high/5.0))*5;
        if(axis.low==axis.high){axis.low-=5;axis.high+=5;}
    }
    return axis;
}
inline bool valid_date(const std::string& date) {
    if(date.size()!=10||date[4]!='-'||date[7]!='-')return false;
    for(size_t i=0;i<date.size();++i)if(i!=4&&i!=7&&!std::isdigit(static_cast<unsigned char>(date[i])))return false;
    int year=0,month=0,day=0;
    if(std::sscanf(date.c_str(),"%d-%d-%d",&year,&month,&day)!=3||year<2000||year>2200||month<1||month>12)return false;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int limit=days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));
    return day>=1&&day<=limit;
}
inline std::string day_label(const std::string& date,const std::array<std::string,3>& current_dates) {
    if(!valid_date(date))return "--";
    const char* names[]={"今天","明天","后天"};
    for(size_t i=0;i<current_dates.size();++i)if(date==current_dates[i])return names[i];
    return date.substr(5,2)+"/"+date.substr(8,2);
}
} // namespace weather_ui
