#pragma once

#include <ctime>
#include <string>
#include <vector>

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
struct WeightDay { std::string date; int grams=-1; time_t timestamp=0; };
struct WeightSnapshot {
    bool ready=false, cached=false;
    std::string baseline_date, latest_date, status;
    int baseline_grams=-1, latest_grams=-1;
    time_t fetched_at=0;
    std::vector<WeightDay> days;
};
void start();
void request_sync();
Snapshot snapshot();
WeightSnapshot weight_snapshot();
void request_weight_refresh();
// Authenticated settings/status handlers only. Caller frees with cJSON_free().
char* status_json();
char* weight_status_json();
} // namespace paperdesk_garmin
