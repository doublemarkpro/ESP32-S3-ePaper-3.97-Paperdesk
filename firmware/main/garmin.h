#pragma once

#include <ctime>
#include <string>

namespace paperdesk_garmin {
struct Snapshot {
    bool configured = false, ready = false, busy = false, cached = false;
    time_t fetched_at = 0; // Last successful daily-summary retrieval.
    time_t last_sync = 0;  // Garmin cloud / watch upload time, zero when unknown.
    time_t attempt_at = 0;
    std::string date, status, activity_type, activity_date;
    int steps = -1, step_goal = -1, body_battery = -1, resting_hr = -1, stress = -1;
    int sleep_score = -1, sleep_seconds = -1, hrv = -1;
    double activity_distance_m = -1, activity_duration_s = -1;
};
void start();
void request_sync();
Snapshot snapshot();
// Authenticated settings/status handlers only. Caller frees with cJSON_free().
char* status_json();
} // namespace paperdesk_garmin
