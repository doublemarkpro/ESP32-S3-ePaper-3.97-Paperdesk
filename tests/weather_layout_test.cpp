#include "weather_layout.h"
#include <iostream>
#include <stdexcept>
#include <utility>

using weather_ui::Axis;
using weather_ui::Range;
using weather_ui::day_label;
using weather_ui::range;
using weather_ui::shared_axis;
using weather_ui::temperature;
using weather_ui::valid_date;

#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #expression); } while (false)

static void test_temperature() {
    const std::pair<const char*, int> valid[] = {
        {"0", 0}, {"-0", 0}, {"-12", -12}, {"+8", 8},
        {"27", 27}, {"-100", -100}, {"100", 100}, {"  -7\t", -7}
    };
    for (auto input : valid) {
        int value = 12345;
        CHECK(temperature(input.first, value));
        CHECK(value == input.second);
    }
    const char* invalid[] = {
        nullptr, "", " ", "--", "NaN", "Infinity", "12C", "12.5", "1e1",
        "0x10", "12 3", "--1", "-101", "101",
        "999999999999999999999999999999999999",
        "-999999999999999999999999999999999999"
    };
    for (auto input : invalid) {
        int value = 12345;
        CHECK(!temperature(input, value));
        CHECK(value == 12345); // An unavailable reading must not become zero.
    }
}

static void test_invalid_ranges() {
    const std::pair<const char*, const char*> invalid[] = {
        {nullptr, "12"}, {"1", nullptr}, {"", "12"}, {"1", ""},
        {"--", "12"}, {"1", "NaN"}, {"12.5", "15"}, {"12", "4"},
        {"100", "-100"}, {"-101", "12"}, {"-10", "101"}
    };
    for (auto input : invalid) {
        auto missing = range(input.first, input.second);
        CHECK(!missing.valid);
        auto none = shared_axis({missing, Range{}, Range{}});
        CHECK(!none.valid);
        // Bad days must not pull an otherwise positive forecast down to 0°C.
        auto mixed = shared_axis({missing, range("12", "18"), range("14", "20")});
        CHECK(mixed.valid);
        CHECK(mixed.low == 10 && mixed.high == 20);
    }
    auto zero = range("0", "0");
    CHECK(zero.valid && zero.low == 0 && zero.high == 0);
    CHECK(shared_axis({zero, Range{}, Range{}}).valid);
    CHECK(!shared_axis({Range{}, Range{}, Range{}}).valid);
}

static void test_signed_axes() {
    auto positive = shared_axis({range("12", "18"), range("14", "22"), range("11", "20")});
    CHECK(positive.valid && positive.low == 10 && positive.high == 25);
    auto negative = shared_axis({range("-13", "-5"), range("-9", "-3"), range("-16", "-8")});
    CHECK(negative.valid && negative.low == -20 && negative.high == 0);
    auto mixed = shared_axis({range("-7", "8"), range("-2", "11"), range("0", "3")});
    CHECK(mixed.valid && mixed.low == -10 && mixed.high == 15);
    CHECK(range("-100", "100").valid);
}

static void test_same_temperature() {
    // Include tick-aligned, non-tick-aligned, zero and extreme values.
    for (int value : {-100, -20, -7, 0, 7, 20, 100}) {
        auto text = std::to_string(value);
        auto one = range(text.c_str(), text.c_str());
        for (const auto& days : {std::array<Range, 3>{one, one, one},
                                 std::array<Range, 3>{one, Range{}, Range{}}}) {
            auto axis = shared_axis(days);
            CHECK(axis.valid && axis.high > axis.low);
            CHECK(axis.low <= value && axis.high >= value);
            int point = axis.project(value, 32, 448);
            CHECK(point >= 32 && point <= 448);
            CHECK(axis.project(one.low, 32, 448) == axis.project(one.high, 32, 448));
        }
    }
    auto zero = shared_axis({range("0", "0"), Range{}, Range{}});
    CHECK(zero.project(0, 32, 448) == 240);
}

static void test_projection() {
    const std::array<Range, 3> days = {range("-10", "0"), range("-5", "5"), range("5", "15")};
    auto axis = shared_axis(days);
    CHECK(axis.valid && axis.low == -10 && axis.high == 15);
    const int left = 40, right = 240;
    CHECK(axis.project(-10, left, right) == left);
    CHECK(axis.project(15, left, right) == right);
    CHECK(axis.project(-100, left, right) == left);
    CHECK(axis.project(100, left, right) == right);
    CHECK(axis.project(0, left, right) == 120);
    CHECK(axis.project(5, left, right) == 160);
    // Each 10-degree daily interval has the same pixel length on this axis.
    for (auto day : days)
        CHECK(axis.project(day.high, left, right) - axis.project(day.low, left, right) == 80);
    int previous = left;
    for (int value = -10; value <= 15; ++value) {
        int point = axis.project(value, left, right);
        CHECK(point >= previous && point <= right);
        previous = point;
    }
    CHECK(axis.project(-10, 32, 449) == 32);
    CHECK(axis.project(15, 32, 449) == 449);
    CHECK(Axis{}.project(0, 32, 448) == 32);
}

static void test_calendar() {
    for (const char* date : {"2000-02-29", "2024-02-29", "2026-02-28", "2026-04-30", "2200-12-31"})
        CHECK(valid_date(date));
    for (const char* date : {"", "--", "2023-02-29", "2100-02-29", "2200-02-29",
                             "2024-02-30", "2026-04-31", "2026-00-10", "2026-13-10",
                             "2026-01-00", "2026-01-32", "2026-1-01", "2026/01/01",
                             "2026-01-01x", "2026-01-01 ", "202a-01-01", "1999-12-31", "2201-01-01"})
        CHECK(!valid_date(date));
}

static void test_cached_dates() {
    const std::array<std::string, 3> current = {"2026-10-29", "2026-10-30", "2026-10-31"};
    CHECK(day_label("2026-10-28", current) == "10/28");
    CHECK(day_label("2026-10-29", current) == "今天");
    CHECK(day_label("2026-10-30", current) == "明天");
    CHECK(day_label("2026-10-31", current) == "后天");
    CHECK(day_label("2026-11-01", current) == "11/01");
    CHECK(day_label("2025-10-29", current) == "10/29");
    CHECK(day_label("", current) == "--");
    CHECK(day_label("2026-02-30", current) == "--");
    // With an uninitialized local calendar, a real forecast retains its date.
    CHECK(day_label("2026-10-29", {"", "", ""}) == "10/29");
}

static void test_date_boundaries() {
    const std::array<std::string, 3> leap = {"2024-02-28", "2024-02-29", "2024-03-01"};
    CHECK(day_label("2024-02-28", leap) == "今天");
    CHECK(day_label("2024-02-29", leap) == "明天");
    CHECK(day_label("2024-03-01", leap) == "后天");
    CHECK(day_label("2023-02-29", leap) == "--");
    const std::array<std::string, 3> nonleap = {"2100-02-28", "2100-03-01", "2100-03-02"};
    CHECK(day_label("2100-03-01", nonleap) == "明天");
    CHECK(day_label("2100-02-29", nonleap) == "--");
    const std::array<std::string, 3> year = {"2026-12-31", "2027-01-01", "2027-01-02"};
    CHECK(day_label("2026-12-31", year) == "今天");
    CHECK(day_label("2027-01-01", year) == "明天");
    CHECK(day_label("2027-01-02", year) == "后天");
    CHECK(day_label("2026-12-30", year) == "12/30");
    CHECK(day_label("2025-12-31", year) == "12/31");
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Expected one named test case\n"; return 2; }
    const std::pair<const char*, void (*)()> cases[] = {
        {"temperature", test_temperature}, {"invalid_ranges", test_invalid_ranges},
        {"signed_axes", test_signed_axes}, {"same_temperature", test_same_temperature},
        {"projection", test_projection}, {"calendar", test_calendar},
        {"cached_dates", test_cached_dates}, {"date_boundaries", test_date_boundaries}
    };
    try {
        for (auto test : cases) if (std::string(argv[1]) == test.first) { test.second(); return 0; }
        std::cerr << "Unknown test case: " << argv[1] << '\n'; return 2;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
