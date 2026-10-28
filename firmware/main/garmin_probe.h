#pragma once

#include <cstddef>

// Callers must authenticate these operations. Never expose imported credentials.
// The JSON contains di_token, di_refresh_token and di_client_id, or action=resume.
bool garmin_probe_begin(const char* json, size_t length);
// The caller owns the returned cJSON allocation and must call cJSON_free().
char* garmin_probe_result_json();
// Explicitly persist the latest complete token pair after a successful refresh.
bool garmin_probe_save();
