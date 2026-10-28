#pragma once
#include <string>

namespace paperdesk_calendar {
struct Date { int year=0,month=0,day=0; };
struct Lunar { bool valid=false; int year=0,month=0,day=0; bool leap=false; };
struct Term { bool valid=false; Date date; const char* name=""; };

bool valid(Date date);
int days_in_month(int year,int month);
// Monday is 0, Sunday is 6; invalid dates return -1.
int weekday_monday0(Date date);
// Number of Monday-first rows in a month: 4, 5, or 6; invalid input returns 0.
int month_rows(int year,int month);
// Returns the first day of the target month, or an invalid Date on overflow.
Date add_months(Date date,int offset);
// Lunar and solar-term data cover Gregorian 1901-01-01 through 2100-12-31.
// Calls outside that interval return an invalid result, without extrapolation.
Lunar lunar(Date date);
// Month names include 月 and optional 闰; Ganzhi names include 年.
std::string lunar_month_name(Lunar date);
std::string lunar_day_name(int day);
std::string ganzhi_year(int lunar_year);
// Zodiac returns only the animal name, without 年.
std::string zodiac(int lunar_year);
Term term_on(Date date);
// The next term must be strictly later than date; never extrapolates the table.
Term next_term(Date date);
} // namespace paperdesk_calendar
