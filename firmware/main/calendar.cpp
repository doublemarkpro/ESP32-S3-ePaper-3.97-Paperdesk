#include "calendar.h"
#include "calendar_data.h"
#include <cstdint>

namespace paperdesk_calendar {
namespace {
constexpr uint32_t day_mask=(1u<<17)-1;
constexpr int days_before_year(int year) {
    int y=year-1;return 365*y+y/4-y/100+y/400;
}
int ordinal(Date date) {
    int result=days_before_year(date.year)+date.day-1;
    for(int month=1;month<date.month;++month)result+=days_in_month(date.year,month);
    return result-days_before_year(1900);
}
Date date_from_ordinal(int day) {
    int absolute=day+days_before_year(1900),lo=1,hi=10000;
    while(lo+1<hi){int middle=(lo+hi)/2;if(days_before_year(middle)<=absolute)lo=middle;else hi=middle;}
    Date result{lo,1,absolute-days_before_year(lo)+1};
    while(result.month<12&&result.day>days_in_month(result.year,result.month)){result.day-=days_in_month(result.year,result.month);++result.month;}
    return result;
}
bool supported(Date date,int& day) {
    if(!valid(date))return false;
    day=ordinal(date);return day>=data::first_day&&day<=data::last_day;
}
template<size_t N> size_t upper_day(const uint32_t (&values)[N],int day) {
    size_t lo=0,hi=N;
    while(lo<hi){size_t middle=(lo+hi)/2;if(int(values[middle]&day_mask)<=day)lo=middle+1;else hi=middle;}
    return lo;
}
Term unpack_term(uint32_t value) {
    unsigned index=value>>17;
    if(index>=24)return {};
    return {true,date_from_ordinal(int(value&day_mask)),data::term_names[index]};
}
int positive_mod(int value,int modulus) {return (value%modulus+modulus)%modulus;}
} // namespace

int days_in_month(int year,int month) {
    if(year<1||year>9999||month<1||month>12)return 0;
    constexpr int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month-1]+(month==2&&year%4==0&&(year%100!=0||year%400==0));
}
bool valid(Date date) {
    int limit=days_in_month(date.year,date.month);return limit>0&&date.day>=1&&date.day<=limit;
}
int weekday_monday0(Date date) {
    if(!valid(date))return -1;
    // 1900-01-01 was Monday; modulo is normalized for earlier public dates.
    return positive_mod(ordinal(date),7);
}
int month_rows(int year,int month) {
    int days=days_in_month(year,month);return days?(weekday_monday0({year,month,1})+days+6)/7:0;
}
Date add_months(Date date,int offset) {
    if(!valid(date))return {};
    int64_t index=int64_t(date.year-1)*12+date.month-1+int64_t(offset);
    if(index<0||index>=9999LL*12)return {};
    return {int(index/12)+1,int(index%12)+1,1};
}
Lunar lunar(Date date) {
    int day=0;if(!supported(date,day))return {};
    size_t next=upper_day(data::month_starts,day);if(!next)return {};
    uint32_t packed=data::month_starts[next-1];int lunar_day=day-int(packed&day_mask)+1;
    if(lunar_day<1||lunar_day>30)return {};
    return {true,1900+int(packed>>22),int((packed>>17)&15),lunar_day,bool((packed>>21)&1)};
}
std::string lunar_month_name(Lunar date) {
    constexpr const char* names[]={"正月","二月","三月","四月","五月","六月","七月","八月","九月","十月","十一月","十二月"};
    if(!date.valid||date.month<1||date.month>12)return "--";
    return std::string(date.leap?"闰":"")+names[date.month-1];
}
std::string lunar_day_name(int day) {
    constexpr const char* names[]={"初一","初二","初三","初四","初五","初六","初七","初八","初九","初十","十一","十二","十三","十四","十五","十六","十七","十八","十九","二十","廿一","廿二","廿三","廿四","廿五","廿六","廿七","廿八","廿九","三十"};
    return day>=1&&day<=30?names[day-1]:"--";
}
std::string ganzhi_year(int year) {
    if(year<1||year>9999)return "--";
    constexpr const char* stems[]={"甲","乙","丙","丁","戊","己","庚","辛","壬","癸"};
    constexpr const char* branches[]={"子","丑","寅","卯","辰","巳","午","未","申","酉","戌","亥"};
    return std::string(stems[positive_mod(year-4,10)])+branches[positive_mod(year-4,12)]+"年";
}
std::string zodiac(int year) {
    if(year<1||year>9999)return "--";
    constexpr const char* animals[]={"鼠","牛","虎","兔","龙","蛇","马","羊","猴","鸡","狗","猪"};
    return animals[positive_mod(year-4,12)];
}
Term term_on(Date date) {
    int day=0;if(!supported(date,day))return {};
    size_t next=upper_day(data::terms,day);
    return next&&int(data::terms[next-1]&day_mask)==day?unpack_term(data::terms[next-1]):Term{};
}
Term next_term(Date date) {
    int day=0;if(!supported(date,day))return {};
    size_t next=upper_day(data::terms,day);
    return next<sizeof(data::terms)/sizeof(data::terms[0])?unpack_term(data::terms[next]):Term{};
}
} // namespace paperdesk_calendar
