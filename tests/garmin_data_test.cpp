#include "garmin_data.h"
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using paperdesk_garmin::Snapshot;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

static Json json(const char* text) {
    Json value(cJSON_Parse(text), cJSON_Delete);
    assert(value);
    return value;
}

static Snapshot previous_day() {
    Snapshot value;
    value.ready = value.configured = true;
    value.date = "2026-10-27";
    value.steps = 5432; value.step_goal = 8000; value.body_battery = 71;
    value.resting_hr = 49; value.stress = 22;
    value.sleep_score = 86; value.sleep_seconds = 27000; value.hrv = 58;
    return value;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string test = argv[1];
    if (test == "cloud_time") {
        // A UTC-labelled value must not be interpreted as local UTC+8.
#ifdef _WIN32
        _putenv("TZ=CST-8");
        _tzset();
#else
        setenv("TZ", "CST-8", 1);
        tzset();
#endif
        for (const char* text : {"1793160000", "1793160000000", "1793160000123",
             "\"2026-10-28T04:00:00\"", "\"2026-10-28T04:00:00.123Z\"",
             "\"2026-10-28 04:00:00+00:00\"", "\"2026-10-28T04:00:00+0000\""}) {
            auto value = json(text);
            assert(garmin_data::cloud_time(value.get()) == time_t(1793160000));
        }
        auto leap = json("\"2024-02-29T00:00:00Z\"");
        assert(garmin_data::cloud_time(leap.get()) == time_t(1709164800));
    } else if (test == "invalid_time") {
        for (const char* text : {"null", "true", "-1", "0", "{}", "\"\"",
             "\"2026-02-29T04:00:00Z\"", "\"2100-02-29T04:00:00Z\"",
             "\"2026-10-28T24:00:00Z\"", "\"2026-10-28T04:60:00Z\"",
             "\"2026-10-28T04:00:00+08:00\"", "\"2026-10-28T04:00:00junk\""}) {
            auto value = json(text);
            assert(garmin_data::cloud_time(value.get()) == 0);
        }
        Json nonfinite(cJSON_CreateNumber(std::numeric_limits<double>::infinity()), cJSON_Delete);
        assert(garmin_data::cloud_time(nonfinite.get()) == 0);
    } else if (test == "rollover") {
        Snapshot previous = previous_day(), current;
        previous.activity_type = "running"; previous.activity_date = "2026-10-25";
        previous.activity_distance_m = 5042.5; previous.activity_duration_s = 1850.5;
        auto summary = json(R"({"calendarDate":"2026-10-28","totalSteps":0,"dailyStepGoal":10000,
            "bodyBatteryMostRecentValue":-1,"restingHeartRate":null,"averageStressLevel":-1,
            "lastSyncTimestampGMT":1793160000123})");
        assert(garmin_data::parse_summary(summary.get(), "2026-10-28", previous, current));
        assert(current.date == "2026-10-28" && current.ready && current.configured);
        assert(current.steps == 0 && current.step_goal == 10000);
        assert(current.body_battery == -1 && current.resting_hr == -1 && current.stress == -1);
        assert(current.sleep_seconds == -1 && current.sleep_score == -1 && current.hrv == -1);
        assert(current.activity_date == "2026-10-25" && current.activity_distance_m == 5042.5);
        assert(current.last_sync == time_t(1793160000) && !current.cached);
        assert(previous.steps == 5432 && previous.sleep_score == 86); // Input is not mutated.
    } else if (test == "same_day") {
        Snapshot previous = previous_day(), current;
        auto summary = json(R"({"calendarDate":"2026-10-27","totalSteps":6000})");
        assert(garmin_data::parse_summary(summary.get(), previous.date, previous, current));
        assert(current.steps == 6000 && current.sleep_score == 86 && current.hrv == 58);
        assert(current.resting_hr == 49 && current.cached); // Kept values are explicitly cached.
        assert(current.last_sync == 0); // Unknown cloud timestamp is not invented from fetch time.
    } else if (test == "invalid_summary") {
        Snapshot previous = previous_day(), unchanged;
        unchanged.date = "unchanged"; unchanged.steps = 123;
        for (const char* text : {"null", "[]", R"({"calendarDate":"2026-10-27","totalSteps":3})",
             R"({"calendarDate":"2026-10-28","privacyProtected":true,"totalSteps":3})",
             R"({"calendarDate":"2026-10-28","totalSteps":-1,"restingHeartRate":null})",
             R"({"calendarDate":"2026-10-28","totalSteps":"8000","dailyStepGoal":true})"}) {
            auto summary = json(text);
            assert(!garmin_data::parse_summary(summary.get(), "2026-10-28", previous, unchanged));
            assert(unchanged.date == "unchanged" && unchanged.steps == 123);
        }
        auto summary = json(R"({"calendarDate":"2026-10-28","totalSteps":0,"dailyStepGoal":true,
            "bodyBatteryMostRecentValue":101,"restingHeartRate":-1,"averageStressLevel":null})");
        Snapshot current;
        assert(garmin_data::parse_summary(summary.get(), "2026-10-28", previous, current));
        assert(current.steps == 0 && current.step_goal == -1 && current.body_battery == -1);
        assert(current.resting_hr == -1 && current.stress == -1);
    } else if (test == "sleep_dates") {
        Snapshot current; current.date = "2026-10-28";
        auto old = json(R"({"dailySleepDTO":{"calendarDate":"2026-10-27","sleepTimeSeconds":27000,
            "sleepScores":{"overall":{"value":86}}}})");
        assert(!garmin_data::parse_sleep(old.get(), current));
        assert(current.sleep_seconds == -1 && current.sleep_score == -1);
        auto fresh = json(R"({"dailySleepDTO":{"calendarDate":"2026-10-28","sleepTimeSeconds":25200,
            "sleepScores":{"overall":{"value":81}}}})");
        assert(garmin_data::parse_sleep(fresh.get(), current));
        assert(current.sleep_seconds == 25200 && current.sleep_score == 81);
        auto partial = json(R"({"dailySleepDTO":{"calendarDate":"2026-10-28","sleepTimeSeconds":25400}})");
        assert(garmin_data::parse_sleep(partial.get(), current));
        assert(current.sleep_seconds == 25400 && current.sleep_score == 81 && current.cached);
        auto missing = json(R"({"dailySleepDTO":{"calendarDate":"2026-10-28","sleepTimeSeconds":-1,
            "sleepScores":{"overall":{"value":null}}}})");
        assert(!garmin_data::parse_sleep(missing.get(), current));
        assert(current.sleep_seconds == 25400 && current.sleep_score == 81);
    } else if (test == "hrv_dates") {
        Snapshot current; current.date = "2026-10-28";
        for (const char* text : {R"({"hrvSummary":{"calendarDate":"2026-10-27","lastNightAvg":59}})",
             R"({"hrvSummary":{"lastNightAvg":59}})",
             R"({"hrvSummary":{"calendarDate":"2026-10-28","lastNightAvg":-1}})"}) {
            auto value = json(text);
            assert(!garmin_data::parse_hrv(value.get(), current));
            assert(current.hrv == -1);
        }
        auto fresh = json(R"({"hrvSummary":{"calendarDate":"2026-10-28","lastNightAvg":61}})");
        assert(garmin_data::parse_hrv(fresh.get(), current) && current.hrv == 61);
    } else if (test == "battery") {
        Snapshot current; current.date = "2026-10-28";
        auto battery = json(R"([
            {"date":"2026-10-27","bodyBatteryValuesArray":[[1793160400000,99]]},
            {"date":"2026-10-28","bodyBatteryValuesArray":[[1793160300000,-1],
              [1793160000000,0],[1793159800000,78],[1793160500000,101],[1793160600000,null]]}
        ])");
        assert(garmin_data::parse_battery(battery.get(), current));
        assert(current.body_battery == 0); // Actual zero is valid; choose timestamp, not array position.
        auto missing = json(R"([{"date":"2026-10-28","bodyBatteryValuesArray":[[1793160700000,-1]]}])");
        assert(!garmin_data::parse_battery(missing.get(), current));
        assert(current.body_battery == 0);
    } else if (test == "activity") {
        Snapshot current; current.date = "2026-10-28";
        auto activity = json(R"([{"activityType":{"typeKey":"running"},"startTimeLocal":"2026-10-25 07:12:00",
            "distance":5042.5,"duration":1850.5}])");
        assert(garmin_data::parse_activity(activity.get(), current));
        assert(current.date == "2026-10-28" && current.activity_date == "2026-10-25");
        assert(current.activity_type == "running" && current.activity_distance_m == 5042.5);
        assert(current.activity_duration_s == 1850.5);
        auto invalid = json(R"([{"activityType":{"typeKey":"running"},"startTimeLocal":"2026-02-30 07:12:00",
            "distance":50,"duration":18}])");
        assert(!garmin_data::parse_activity(invalid.get(), current));
        assert(current.activity_date == "2026-10-25");
        auto empty = json("[]");
        assert(garmin_data::parse_activity(empty.get(), current));
        assert(current.activity_type.empty() && current.activity_date.empty());
        assert(current.activity_distance_m == -1 && current.activity_duration_s == -1);
    } else {
        return 2;
    }
    std::cout << test << " passed\n";
}
