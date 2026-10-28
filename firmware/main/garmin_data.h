#pragma once
#include "garmin.h"
#include "cJSON.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>

namespace garmin_data {
inline bool ascii_text(const char* value, size_t maximum) {
    if (!value || !*value || std::strlen(value) > maximum) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (*p < 0x21 || *p > 0x7e) return false;
    return true;
}
inline const cJSON* field(const cJSON* json, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(json, key);
}

inline bool get_number(const cJSON* value, double& number, double maximum) {
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > maximum) return false;
    number = value->valuedouble;
    return true;
}

inline bool set_metric(const cJSON* value, int& target, int maximum) {
    double number;
    if (!get_number(value, number, maximum)) return false;
    target = int(std::lround(number));
    return true;
}

inline bool date_valid(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    int year, month, day;
    if (std::sscanf(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3 ||
        year < 2000 || year > 2100 || month < 1 || month > 12) return false;
    static constexpr int Days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int maximum = Days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= maximum;
}

inline bool matches_date(const cJSON* value, const std::string& date) {
    return cJSON_IsString(value) && date == value->valuestring;
}

// The field is documented as GMT. Do not apply the device's UTC+8 timezone twice.
inline time_t cloud_time(const cJSON* value) {
    if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
        double epoch = value->valuedouble;
        if (epoch > 100000000000.0) epoch /= 1000;
        return epoch >= 946684800.0 && epoch < 4133980800.0 ? time_t(epoch) : 0;
    }
    if (!cJSON_IsString(value)) return 0;
    std::string text = value->valuestring;
    if (text.size() < 19 || text.size() > 35 || !date_valid(text.substr(0, 10)) ||
        (text[10] != 'T' && text[10] != ' ') || text[13] != ':' || text[16] != ':') return 0;
    int year, month, day, hour, minute, second;
    if (std::sscanf(text.c_str(), "%d-%d-%d%*c%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return 0;
    for (size_t i : {size_t(11),size_t(12),size_t(14),size_t(15),size_t(17),size_t(18)})
        if (text[i] < '0' || text[i] > '9') return 0;
    size_t suffix = 19;
    if (suffix < text.size() && text[suffix] == '.') {
        size_t start = ++suffix;
        while (suffix < text.size() && text[suffix] >= '0' && text[suffix] <= '9') ++suffix;
        if (suffix == start) return 0;
    }
    // GMT-labelled input may be unzoned, Z or an explicit zero offset only.
    std::string zone = text.substr(suffix);
    if (!zone.empty() && zone != "Z" && zone != "+00:00" && zone != "+0000") return 0;
    year -= month <= 2;
    int era = year / 400;
    unsigned yoe = unsigned(year - era * 400);
    unsigned doy = (153 * unsigned(month + (month > 2 ? -3 : 9)) + 2) / 5 + unsigned(day) - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = int64_t(era) * 146097 + doe - 719468;
    return time_t(days * 86400 + hour * 3600 + minute * 60 + second);
}

inline bool parse_summary(const cJSON* json, const std::string& date,
                          const paperdesk_garmin::Snapshot& previous,
                          paperdesk_garmin::Snapshot& out) {
    if (!date_valid(date) || !cJSON_IsObject(json) ||
        cJSON_IsTrue(field(json, "privacyProtected")) ||
        !matches_date(field(json, "calendarDate"), date)) return false;
    auto value = previous.ready && previous.date == date ? previous : paperdesk_garmin::Snapshot{};
    // Latest activity has its own visible date and remains valid across midnight.
    if (previous.ready && previous.date != date && date_valid(previous.activity_date)) {
        value.activity_type = previous.activity_type; value.activity_date = previous.activity_date;
        value.activity_distance_m = previous.activity_distance_m;
        value.activity_duration_s = previous.activity_duration_s;
    }
    value.date = date; value.ready = true; value.configured = true; value.cached = false;
    value.last_sync = cloud_time(field(json, "lastSyncTimestampGMT"));
    unsigned present = 0;
    auto metric = [&](const char* name, int& target, int maximum) {
        if (set_metric(field(json, name), target, maximum)) ++present;
        else if (target >= 0) value.cached = true;
    };
    metric("totalSteps", value.steps, 1000000);
    metric("dailyStepGoal", value.step_goal, 1000000);
    metric("bodyBatteryMostRecentValue", value.body_battery, 100);
    metric("restingHeartRate", value.resting_hr, 300);
    metric("averageStressLevel", value.stress, 100);
    if (!present) return false;
    out = std::move(value);
    return true;
}

inline bool parse_sleep(const cJSON* json, paperdesk_garmin::Snapshot& value) {
    auto sleep = field(json, "dailySleepDTO");
    if (!matches_date(field(sleep, "calendarDate"), value.date)) return false;
    bool seconds = set_metric(field(sleep, "sleepTimeSeconds"), value.sleep_seconds, 172800);
    bool score = set_metric(field(field(field(sleep, "sleepScores"), "overall"), "value"), value.sleep_score, 100);
    if ((!seconds && value.sleep_seconds >= 0) || (!score && value.sleep_score >= 0)) value.cached = true;
    return seconds || score;
}

inline bool parse_battery(const cJSON* json, paperdesk_garmin::Snapshot& value) {
    if (!cJSON_IsArray(json)) return false;
    int latest = -1;
    double latest_time = -1;
    const cJSON* day = nullptr;
    cJSON_ArrayForEach(day, json) {
        if (!matches_date(field(day, "date"), value.date)) continue;
        auto values = field(day, "bodyBatteryValuesArray");
        const cJSON* row = nullptr;
        cJSON_ArrayForEach(row, values) {
            double stamp;
            int level = -1;
            if (cJSON_IsArray(row) && get_number(cJSON_GetArrayItem(row, 0), stamp, 1e15) &&
                set_metric(cJSON_GetArrayItem(row, 1), level, 100) && stamp >= latest_time) {
                latest_time = stamp; latest = level;
            }
        }
    }
    if (latest < 0) return false;
    value.body_battery = latest;
    return true;
}

inline bool parse_activity(const cJSON* json, paperdesk_garmin::Snapshot& value) {
    if (!cJSON_IsArray(json)) return false;
    // A valid empty list means there is no recorded activity, not a transport failure.
    if (cJSON_GetArraySize(json) == 0) {
        value.activity_type.clear(); value.activity_date.clear();
        value.activity_distance_m = -1; value.activity_duration_s = -1;
        return true;
    }
    auto activity = cJSON_GetArrayItem(json, 0);
    auto type = field(field(activity, "activityType"), "typeKey");
    auto started = field(activity, "startTimeLocal");
    if (!cJSON_IsString(type) || !ascii_text(type->valuestring, 64) ||
        !cJSON_IsString(started) || std::strlen(started->valuestring) < 10) return false;
    std::string date(started->valuestring, 10);
    if (!date_valid(date)) return false;
    double duration = -1, distance = -1;
    bool has_duration = get_number(field(activity, "duration"), duration, 604800);
    bool has_distance = get_number(field(activity, "distance"), distance, 10000000);
    if (!has_duration && !has_distance) return false;
    value.activity_type = type->valuestring;
    value.activity_date = date;
    value.activity_duration_s = duration;
    value.activity_distance_m = distance;
    return true;
}

inline bool parse_hrv(const cJSON* json, paperdesk_garmin::Snapshot& value) {
    auto summary = field(json, "hrvSummary");
    return matches_date(field(summary, "calendarDate"), value.date) &&
           set_metric(field(summary, "lastNightAvg"), value.hrv, 1000);
}

} // namespace garmin_data
