#include "calendar.h"
#include <climits>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace paperdesk_calendar;

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #expression); } while (false)

static bool same(Date actual, Date expected) {
    return actual.year == expected.year && actual.month == expected.month && actual.day == expected.day;
}

static void expect_lunar(Date date, int year, int month, int day, bool leap = false) {
    auto actual = lunar(date);
    CHECK(actual.valid);
    CHECK(actual.year == year && actual.month == month && actual.day == day && actual.leap == leap);
}

static void expect_term(Term actual, Date date, const char* name) {
    CHECK(actual.valid);
    CHECK(same(actual.date, date));
    CHECK(std::string(actual.name) == name);
}

static void test_gregorian() {
    CHECK(days_in_month(2024, 2) == 29);
    CHECK(days_in_month(2026, 2) == 28);
    CHECK(days_in_month(1900, 2) == 28);
    CHECK(days_in_month(2000, 2) == 29);
    CHECK(days_in_month(2100, 2) == 28);
    CHECK(days_in_month(2026, 4) == 30);
    CHECK(days_in_month(2026, 12) == 31);
    CHECK(valid({1, 1, 1}) && valid({9999, 12, 31}));
    CHECK(valid({2000, 2, 29}) && valid({2024, 2, 29}));
    for (Date date : {Date{}, Date{0, 1, 1}, Date{10000, 1, 1}, Date{2026, 0, 1},
                      Date{2026, 13, 1}, Date{2026, 1, 0}, Date{2026, 1, 32},
                      Date{2026, 4, 31}, Date{2023, 2, 29}, Date{1900, 2, 29}, Date{2100, 2, 29}})
        CHECK(!valid(date));
    CHECK(days_in_month(2026, 0) == 0 && days_in_month(2026, 13) == 0);
    CHECK(days_in_month(0, 1) == 0 && days_in_month(10000, 1) == 0);
}

static void test_month_layout() {
    // Fixed calendars, not a duplicate of the production month_rows formula.
    CHECK(month_rows(2021, 2) == 4); // Monday 1st, Sunday 28th.
    CHECK(weekday_monday0({2021, 2, 1}) == 0 && weekday_monday0({2021, 2, 28}) == 6);
    CHECK(month_rows(2026, 10) == 5); // Thursday 1st, Saturday 31st.
    CHECK(weekday_monday0({2026, 10, 1}) == 3 && weekday_monday0({2026, 10, 31}) == 5);
    CHECK(month_rows(2026, 11) == 6); // Sunday 1st needs its own first row.
    CHECK(weekday_monday0({2026, 11, 1}) == 6 && weekday_monday0({2026, 11, 30}) == 0);
    CHECK(month_rows(2026, 3) == 6);
    CHECK(month_rows(2024, 2) == 5);
    CHECK(month_rows(2026, 0) == 0 && month_rows(2026, 13) == 0);
    CHECK(month_rows(0, 1) == 0 && month_rows(10000, 1) == 0);
}

static void test_weekdays() {
    CHECK(weekday_monday0({1970, 1, 1}) == 3);
    CHECK(weekday_monday0({2000, 1, 1}) == 5);
    CHECK(weekday_monday0({2024, 2, 29}) == 3);
    CHECK(weekday_monday0({2026, 10, 28}) == 2);
    CHECK(weekday_monday0({2027, 1, 1}) == 4);
    CHECK(weekday_monday0({2026, 2, 29}) == -1);
    CHECK(weekday_monday0({}) == -1);
}

static void test_add_months() {
    CHECK(same(add_months({2026, 10, 28}, 0), {2026, 10, 1}));
    CHECK(same(add_months({2026, 10, 28}, 1), {2026, 11, 1}));
    CHECK(same(add_months({2026, 12, 31}, 1), {2027, 1, 1}));
    CHECK(same(add_months({2027, 1, 31}, -1), {2026, 12, 1}));
    CHECK(same(add_months({2024, 2, 29}, 12), {2025, 2, 1}));
    CHECK(same(add_months({2026, 12, 1}, 13), {2028, 1, 1}));
    CHECK(same(add_months({2026, 1, 1}, -13), {2024, 12, 1}));
    CHECK(!valid(add_months({1, 1, 1}, -1)));
    CHECK(!valid(add_months({9999, 12, 31}, 1)));
    CHECK(!valid(add_months({2026, 10, 28}, INT_MAX)));
    CHECK(!valid(add_months({2026, 10, 28}, INT_MIN)));
    CHECK(!valid(add_months({2026, 2, 30}, 1)));
}

// The following fixed lunar/term expectations were checked against HKO's
// original year tables and CSV, rather than derived from the generated data:
// https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T2023c.txt
// https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T2024c.txt
// https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T2027c.txt
// https://www.hko.gov.hk/tc/gts/time/calendar/text/files/T2033c.txt
// https://data.weather.gov.hk/weatherAPI/hko_data/calendar/nongli_calendar_2026.csv
// https://data.weather.gov.hk/weatherAPI/hko_data/calendar/nongli_calendar_2025.csv
static void test_today() {
    expect_lunar({2026, 10, 28}, 2026, 9, 19);
    auto today = lunar({2026, 10, 28});
    CHECK(lunar_month_name(today) == "九月");
    CHECK(lunar_day_name(today.day) == "十九");
    CHECK(weekday_monday0({2026, 10, 28}) == 2);
    CHECK(ganzhi_year(today.year) == "丙午年");
    CHECK(zodiac(today.year) == "马");
}

static void test_spring_festival() {
    expect_lunar({2023, 1, 22}, 2023, 1, 1);
    expect_lunar({2024, 2, 9}, 2023, 12, 30);
    expect_lunar({2024, 2, 10}, 2024, 1, 1);
    expect_lunar({2025, 1, 29}, 2025, 1, 1);
    expect_lunar({2026, 2, 17}, 2026, 1, 1);
    expect_lunar({2027, 2, 6}, 2027, 1, 1);
    CHECK(lunar_month_name(lunar({2026, 2, 17})) == "正月");
    CHECK(lunar_day_name(1) == "初一");
    CHECK(ganzhi_year(lunar({2024, 2, 9}).year) == "癸卯年");
    CHECK(ganzhi_year(lunar({2024, 2, 10}).year) == "甲辰年");
}

static void test_leap_month() {
    expect_lunar({2023, 3, 21}, 2023, 2, 30);
    expect_lunar({2023, 3, 22}, 2023, 2, 1, true);
    expect_lunar({2023, 4, 20}, 2023, 3, 1);
    expect_lunar({2025, 7, 25}, 2025, 6, 1, true);
    CHECK(lunar_month_name(lunar({2023, 3, 22})) == "闰二月");
    CHECK(lunar_month_name(lunar({2025, 7, 25})) == "闰六月");
    CHECK(lunar_month_name(lunar({2023, 3, 21})) == "二月");
    // 2033's unusual leap eleventh month has historically exposed bad tables.
    expect_lunar({2033, 11, 22}, 2033, 11, 1);
    expect_lunar({2033, 12, 21}, 2033, 11, 30);
    expect_lunar({2033, 12, 22}, 2033, 11, 1, true);
    expect_lunar({2033, 12, 31}, 2033, 11, 10, true);
    CHECK(lunar_month_name(lunar({2033, 12, 22})) == "闰十一月");
}

static void test_solar_terms() {
    expect_term(term_on({2026, 10, 23}), {2026, 10, 23}, "霜降");
    expect_term(term_on({2026, 11, 7}), {2026, 11, 7}, "立冬");
    CHECK(!term_on({2026, 10, 28}).valid);
    expect_term(next_term({2026, 10, 22}), {2026, 10, 23}, "霜降");
    expect_term(next_term({2026, 10, 23}), {2026, 11, 7}, "立冬");
    expect_term(next_term({2026, 10, 28}), {2026, 11, 7}, "立冬");
}

static void test_term_year_boundary() {
    expect_term(next_term({2026, 12, 31}), {2027, 1, 5}, "小寒");
    expect_term(term_on({2027, 1, 5}), {2027, 1, 5}, "小寒");
    expect_term(next_term({2027, 1, 5}), {2027, 1, 20}, "大寒");
    CHECK(!next_term({2100, 12, 31}).valid);
}

static void test_data_boundaries() {
    CHECK(lunar({1901, 1, 1}).valid);
    CHECK(lunar({2100, 12, 31}).valid);
    CHECK(next_term({1901, 1, 1}).valid);
    // Gregorian dates still work outside the finite lunar/term table.
    for (Date date : {Date{1900, 12, 31}, Date{2101, 1, 1}, Date{1, 1, 1}, Date{9999, 12, 31}}) {
        CHECK(valid(date));
        CHECK(!lunar(date).valid);
        CHECK(!term_on(date).valid);
        CHECK(!next_term(date).valid);
    }
    for (Date date : {Date{}, Date{2026, 2, 30}, Date{2026, 13, 1}}) {
        CHECK(!lunar(date).valid);
        CHECK(!term_on(date).valid);
        CHECK(!next_term(date).valid);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Expected one named test case\n"; return 2; }
    const std::pair<const char*, void (*)()> cases[] = {
        {"gregorian", test_gregorian}, {"month_layout", test_month_layout},
        {"weekdays", test_weekdays}, {"add_months", test_add_months},
        {"today", test_today}, {"spring_festival", test_spring_festival},
        {"leap_month", test_leap_month}, {"solar_terms", test_solar_terms},
        {"term_year_boundary", test_term_year_boundary}, {"data_boundaries", test_data_boundaries}
    };
    try {
        for (auto test : cases) if (std::string(argv[1]) == test.first) { test.second(); return 0; }
        std::cerr << "Unknown test case: " << argv[1] << '\n'; return 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
