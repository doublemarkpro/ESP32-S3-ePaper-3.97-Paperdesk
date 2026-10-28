#include "garmin_probe.h"
#include "garmin.h"
#include "garmin_data.h"
#include "paperdesk.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {
constexpr size_t MaxInput = 32 * 1024;
constexpr size_t MaxResponse = 64 * 1024;
constexpr size_t MaxHealthResponse = 512 * 1024;
constexpr int64_t SyncIntervalUs = 1800000000;
constexpr int64_t RunTimeoutUs = 120000000;
constexpr const char* ApiHost = "https://connectapi.garmin.cn";
constexpr const char* TokenUrl = "https://diauth.garmin.cn/di-oauth2-service/oauth/token";
constexpr const char* StoreKey = "tokens";

struct Credentials {
    std::string access, refresh, client;
};
struct Check {
    int status = 0;
    size_t bytes = 0;
    bool complete = false;
    bool valid = false;
};
struct Metric {
    const char* name;
    double value = 0;
    bool present = false;
};
constexpr std::array<const char*, 8> MetricNames = {{
    "totalSteps", "dailyStepGoal", "restingHeartRate", "bodyBatteryMostRecentValue",
    "averageStressLevel", "activeKilocalories", "totalKilocalories", "totalDistanceMeters"
}};
struct State {
    bool busy = false;
    bool core_passed = false;
    bool refreshed = false;
    bool saved = false;
    bool resumed = false;
    const char* phase = "idle";
    const char* error = "";
    time_t updated_at = 0;
    std::array<Check, 4> checks{};
    std::array<Metric, 8> metrics{};
    Credentials credentials;
};
std::mutex state_mutex;
State state;
struct Service {
    paperdesk_garmin::Snapshot current;
    bool started = false, busy = false, requested = false;
    bool reauth = false, backoff = false, persist_pending = false;
    int64_t next_due = 0, last_request = 0;
};
Service service;

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using Buffer = std::unique_ptr<char, decltype(&std::free)>;

bool ascii_value(const char* value, size_t limit) {
    if (!value || !*value || std::strlen(value) > limit) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p)
        if (*p < 0x21 || *p > 0x7e) return false;
    return true;
}

bool credentials_from_json(const cJSON* json, Credentials& out) {
    if (!cJSON_IsObject(json)) return false;
    auto region = cJSON_GetObjectItemCaseSensitive(json, "domain");
    if (region && (!cJSON_IsString(region) || std::strcmp(region->valuestring, "garmin.cn")))
        return false;
    auto access = cJSON_GetObjectItemCaseSensitive(json, "di_token");
    auto refresh = cJSON_GetObjectItemCaseSensitive(json, "di_refresh_token");
    auto client = cJSON_GetObjectItemCaseSensitive(json, "di_client_id");
    if (!cJSON_IsString(access) || !cJSON_IsString(refresh) || !cJSON_IsString(client) ||
        !ascii_value(access->valuestring, 8192) || !ascii_value(refresh->valuestring, 4096) ||
        !ascii_value(client->valuestring, 128)) return false;
    // client_id is used in Basic auth and form data; reject Basic delimiters.
    if (std::strchr(client->valuestring, ':')) return false;
    out = {access->valuestring, refresh->valuestring, client->valuestring};
    return true;
}

std::string encode(const std::string& value) {
    static constexpr char Hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            encoded.push_back(char(c));
        else {
            encoded.push_back('%');
            encoded.push_back(Hex[c >> 4]);
            encoded.push_back(Hex[c & 15]);
        }
    }
    return encoded;
}

std::string basic_auth(const std::string& client) {
    std::string plain = client + ":";
    std::array<unsigned char, 256> output{};
    size_t written = 0;
    if (mbedtls_base64_encode(output.data(), output.size(), &written,
            reinterpret_cast<const unsigned char*>(plain.data()), plain.size()) != 0) return {};
    return "Basic " + std::string(reinterpret_cast<const char*>(output.data()), written);
}

void phase(const char* value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    state.phase = value;
}

void record(size_t index, const Check& check) {
    std::lock_guard<std::mutex> lock(state_mutex);
    state.checks[index] = check;
}

void finish(bool success, const char* error) {
    std::lock_guard<std::mutex> lock(state_mutex);
    state.core_passed = success;
    state.phase = success ? "complete" : "failed";
    state.error = error;
    state.updated_at = time(nullptr);
    state.busy = false;
}

bool native_headers(esp_http_client_handle_t http, const std::string& authorization, bool post) {
    const std::pair<const char*, const char*> headers[] = {
        {"User-Agent", "GCM-Android-5.23"},
        {"X-Garmin-User-Agent", "com.garmin.android.apps.connectmobile/5.23; ; Google/sdk_gphone64_arm64/google; Android/33; Dalvik/2.1.0"},
        {"X-Garmin-Paired-App-Version", "10861"},
        {"X-Garmin-Client-Platform", "Android"},
        {"X-App-Ver", "10861"}, {"X-Lang", "en"}, {"X-GCExperience", "GC5"},
        {"Accept-Language", "en-US,en;q=0.9"}, {"Accept", "application/json"},
        {"Accept-Encoding", "identity"}, {"Cache-Control", "no-cache"}
    };
    for (const auto& header : headers)
        if (esp_http_client_set_header(http, header.first, header.second) != ESP_OK) return false;
    if (esp_http_client_set_header(http, "Authorization", authorization.c_str()) != ESP_OK) return false;
    return !post || esp_http_client_set_header(http, "Content-Type", "application/x-www-form-urlencoded") == ESP_OK;
}

// All URLs are assembled inside this translation unit from fixed Garmin hosts.
// Read into a bounded PSRAM allocation; neither headers nor error bodies are logged.
Json request(const std::string& url, const std::string& auth, const std::string* post,
             int64_t run_deadline, Check& check, size_t maximum = MaxResponse) {
    Json result(nullptr, cJSON_Delete);
    if (esp_timer_get_time() >= run_deadline) return result;
    if (!maximum || maximum > MaxHealthResponse) return result;
    Buffer body(static_cast<char*>(heap_caps_malloc(maximum + 1,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), std::free);
    if (!body) return result;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.timeout_ms = 10000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 12288;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = true;
    auto http = esp_http_client_init(&config);
    if (!http) return result;
    bool transport_ok = native_headers(http, auth, post != nullptr);
    if (transport_ok) transport_ok = esp_http_client_open(http, post ? int(post->size()) : 0) == ESP_OK;
    if (transport_ok && post) {
        size_t sent = 0;
        while (sent < post->size() && esp_timer_get_time() < run_deadline) {
            int count = esp_http_client_write(http, post->data() + sent, int(post->size() - sent));
            if (count <= 0) break;
            sent += size_t(count);
        }
        transport_ok = sent == post->size();
    }
    if (transport_ok) {
        int64_t length = esp_http_client_fetch_headers(http);
        check.status = esp_http_client_get_status_code(http);
        transport_ok = length >= 0 && length <= int64_t(maximum);
    }
    while (transport_ok && esp_timer_get_time() < run_deadline) {
        // Leave one byte for the terminator. When full, only a complete body is accepted.
        if (check.bytes == maximum) {
            check.complete = esp_http_client_is_complete_data_received(http);
            break;
        }
        int count = esp_http_client_read(http, body.get() + check.bytes,
                                        int(std::min(size_t(4096), maximum - check.bytes)));
        if (count < 0) { transport_ok = false; break; }
        if (count == 0) {
            check.complete = esp_http_client_is_complete_data_received(http);
            break;
        }
        check.bytes += size_t(count);
        vTaskDelay(1);
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (transport_ok && check.complete && check.status == 200 && check.bytes) {
        body.get()[check.bytes] = '\0';
        result.reset(cJSON_ParseWithLength(body.get(), check.bytes + 1));
        if (!cJSON_IsObject(result.get()) && !cJSON_IsArray(result.get())) result.reset();
    }
    // Token response buffers are temporary and are never retained in the report.
    std::memset(body.get(), 0, check.bytes);
    return result;
}

bool summary_valid(const cJSON* json, const char* date, std::array<Metric, 8>& metrics) {
    if (!cJSON_IsObject(json)) return false;
    auto privacy = cJSON_GetObjectItemCaseSensitive(json, "privacyProtected");
    if (cJSON_IsTrue(privacy)) return false;
    auto calendar = cJSON_GetObjectItemCaseSensitive(json, "calendarDate");
    if (!cJSON_IsString(calendar) || std::strcmp(calendar->valuestring, date)) return false;
    size_t count = 0;
    for (size_t i = 0; i < MetricNames.size(); ++i) {
        metrics[i].name = MetricNames[i];
        auto value = cJSON_GetObjectItemCaseSensitive(json, MetricNames[i]);
        if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble) && value->valuedouble >= 0) {
            metrics[i].value = value->valuedouble;
            metrics[i].present = true;
            ++count;
        }
    }
    return count > 0;
}

bool load_credentials(Credentials& credentials) {
    nvs_handle_t handle;
    if (nvs_open_from_partition("storage", "garmin", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = 0;
    bool ok = nvs_get_blob(handle, StoreKey, nullptr, &length) == ESP_OK && length > 0 && length <= MaxInput;
    std::string body;
    if (ok) {
        body.resize(length);
        ok = nvs_get_blob(handle, StoreKey, body.data(), &length) == ESP_OK;
    }
    nvs_close(handle);
    if (!ok) return false;
    Json json(cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    ok = credentials_from_json(json.get(), credentials);
    std::fill(body.begin(), body.end(), '\0');
    return ok;
}

bool save_credentials(const Credentials& credentials) {
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "domain", "garmin.cn") ||
        !cJSON_AddStringToObject(json.get(), "di_token", credentials.access.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "di_refresh_token", credentials.refresh.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "di_client_id", credentials.client.c_str())) return false;
    char* body = cJSON_PrintUnformatted(json.get());
    if (!body) return false;
    nvs_handle_t handle;
    bool ok = nvs_open_from_partition("storage", "garmin", NVS_READWRITE, &handle) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(handle, StoreKey, body, std::strlen(body) + 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    std::memset(body, 0, std::strlen(body));
    cJSON_free(body);
    return ok;
}

using namespace garmin_data;

Json snapshot_json(const paperdesk_garmin::Snapshot& value) {
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json) return json;
    cJSON_AddNumberToObject(json.get(), "schema", 1);
    cJSON_AddBoolToObject(json.get(), "configured", value.configured);
    cJSON_AddBoolToObject(json.get(), "ready", value.ready);
    cJSON_AddBoolToObject(json.get(), "busy", value.busy);
    cJSON_AddBoolToObject(json.get(), "cached", value.cached);
    cJSON_AddNumberToObject(json.get(), "fetched_at", double(value.fetched_at));
    cJSON_AddNumberToObject(json.get(), "last_sync", double(value.last_sync));
    cJSON_AddNumberToObject(json.get(), "attempt_at", double(value.attempt_at));
    cJSON_AddStringToObject(json.get(), "date", value.date.c_str());
    cJSON_AddStringToObject(json.get(), "status", value.status.c_str());
    cJSON_AddStringToObject(json.get(), "activity_type", value.activity_type.c_str());
    cJSON_AddStringToObject(json.get(), "activity_date", value.activity_date.c_str());
    const std::pair<const char*, double> fields[] = {
        {"steps", value.steps}, {"step_goal", value.step_goal}, {"body_battery", value.body_battery},
        {"resting_hr", value.resting_hr}, {"stress", value.stress}, {"sleep_score", value.sleep_score},
        {"sleep_seconds", value.sleep_seconds}, {"hrv", value.hrv},
        {"activity_distance_m", value.activity_distance_m}, {"activity_duration_s", value.activity_duration_s}
    };
    for (const auto& item : fields) cJSON_AddNumberToObject(json.get(), item.first, item.second);
    return json;
}

bool save_summary(const paperdesk_garmin::Snapshot& value) {
    auto json = snapshot_json(value);
    char* body = json ? cJSON_PrintUnformatted(json.get()) : nullptr;
    if (!body) return false;
    nvs_handle_t handle;
    bool ok = nvs_open_from_partition("storage", "garmin", NVS_READWRITE, &handle) == ESP_OK;
    if (ok) {
        ok = nvs_set_blob(handle, "summary", body, std::strlen(body) + 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    cJSON_free(body);
    return ok;
}

bool load_summary(paperdesk_garmin::Snapshot& value) {
    nvs_handle_t handle;
    if (nvs_open_from_partition("storage", "garmin", NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = 0;
    bool ok = nvs_get_blob(handle, "summary", nullptr, &length) == ESP_OK && length > 0 && length <= 8192;
    std::string body;
    if (ok) { body.resize(length); ok = nvs_get_blob(handle, "summary", body.data(), &length) == ESP_OK; }
    nvs_close(handle);
    if (!ok) return false;
    Json json(cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    auto date = field(json.get(), "date");
    auto version = field(json.get(), "schema");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsString(date) ||
        !date_valid(date->valuestring) || !cJSON_IsTrue(field(json.get(), "ready"))) return false;
    value.date = date->valuestring;
    value.fetched_at = cloud_time(field(json.get(), "fetched_at"));
    if (!value.fetched_at) return false;
    value.last_sync = cloud_time(field(json.get(), "last_sync"));
    value.attempt_at = cloud_time(field(json.get(), "attempt_at"));
    set_metric(field(json.get(), "steps"), value.steps, 1000000);
    set_metric(field(json.get(), "step_goal"), value.step_goal, 1000000);
    set_metric(field(json.get(), "body_battery"), value.body_battery, 100);
    set_metric(field(json.get(), "resting_hr"), value.resting_hr, 300);
    set_metric(field(json.get(), "stress"), value.stress, 100);
    set_metric(field(json.get(), "sleep_score"), value.sleep_score, 100);
    set_metric(field(json.get(), "sleep_seconds"), value.sleep_seconds, 172800);
    set_metric(field(json.get(), "hrv"), value.hrv, 1000);
    get_number(field(json.get(), "activity_distance_m"), value.activity_distance_m, 10000000);
    get_number(field(json.get(), "activity_duration_s"), value.activity_duration_s, 604800);
    auto type = field(json.get(), "activity_type"), activity_date = field(json.get(), "activity_date");
    if (cJSON_IsString(type) && std::strlen(type->valuestring) <= 64) value.activity_type = type->valuestring;
    if (cJSON_IsString(activity_date) && date_valid(activity_date->valuestring)) value.activity_date = activity_date->valuestring;
    value.ready = true;
    value.cached = true;
    value.status = "已载入佳明简报缓存";
    return true;
}

const char* request_failure(const Check& check) {
    if (check.status == 401 || check.status == 403) return "佳明授权失效，请重新授权";
    if (check.status == 429) return "佳明请求受限，30分钟后重试";
    return "佳明连接失败，30分钟后重试";
}

struct SyncResult {
    paperdesk_garmin::Snapshot value;
    bool summary_ok = false, failed = false, reauth = false;
};

SyncResult synchronize(Credentials credentials, const paperdesk_garmin::Snapshot& previous) {
    SyncResult out;
    out.value = previous;
    out.value.configured = true;
    out.value.busy = true;
    out.value.attempt_at = time(nullptr);
    auto fail = [&](const char* reason, bool reauth = false) {
        out.failed = true; out.reauth = reauth;
        out.value.status = reason; out.value.cached = out.value.ready;
    };
    const int64_t deadline = esp_timer_get_time() + 180000000;
    bool pending;
    { std::lock_guard<std::mutex> lock(state_mutex); pending = service.persist_pending; }
    if (pending) {
        bool saved = save_credentials(credentials);
        { std::lock_guard<std::mutex> lock(state_mutex); state.saved = saved; service.persist_pending = !saved; }
        if (!saved) { fail("授权保存失败，30分钟后重试"); return out; }
    }
    Check refresh_check;
    std::string form = "grant_type=refresh_token&client_id=" + encode(credentials.client) +
                       "&refresh_token=" + encode(credentials.refresh);
    auto refreshed = request(TokenUrl, basic_auth(credentials.client), &form, deadline, refresh_check);
    std::fill(form.begin(), form.end(), '\0');
    auto access = field(refreshed.get(), "access_token"), refresh = field(refreshed.get(), "refresh_token");
    if (!cJSON_IsString(access) || !ascii_value(access->valuestring, 8192) ||
        (refresh && (!cJSON_IsString(refresh) || !ascii_value(refresh->valuestring, 4096)))) {
        bool reauth = refresh_check.status == 400 || refresh_check.status == 401 || refresh_check.status == 403;
        fail(reauth ? "佳明授权失效，请重新授权" : request_failure(refresh_check), reauth);
        return out;
    }
    credentials.access = access->valuestring;
    if (refresh) credentials.refresh = refresh->valuestring;
    refreshed.reset();
    { std::lock_guard<std::mutex> lock(state_mutex);
      state.credentials = credentials; state.refreshed = true; state.saved = false; service.persist_pending = true; }
    // Commit the rotated pair before reading health data or allowing another refresh.
    bool saved = save_credentials(credentials);
    { std::lock_guard<std::mutex> lock(state_mutex); state.saved = saved; service.persist_pending = !saved; }
    if (!saved) { fail("授权保存失败，30分钟后重试"); return out; }
    Check profile_check;
    auto profile = request(std::string(ApiHost) + "/userprofile-service/socialProfile",
                           "Bearer " + credentials.access, nullptr, deadline, profile_check);
    auto display = field(profile.get(), "displayName");
    if (!cJSON_IsString(display) || !display->valuestring[0] || std::strlen(display->valuestring) > 256) {
        fail(request_failure(profile_check), profile_check.status == 401 || profile_check.status == 403); return out;
    }
    std::string display_path = encode(display->valuestring);
    profile.reset();
    time_t now = time(nullptr); tm local{}; localtime_r(&now, &local);
    char date[16]; strftime(date, sizeof date, "%Y-%m-%d", &local);
    Check summary_check;
    auto summary = request(std::string(ApiHost) + "/usersummary-service/usersummary/daily/" + display_path +
                           "?calendarDate=" + date, "Bearer " + credentials.access, nullptr, deadline, summary_check);
    paperdesk_garmin::Snapshot fresh;
    if (!parse_summary(summary.get(), date, previous, fresh)) {
        fail(request_failure(summary_check), summary_check.status == 401 || summary_check.status == 403); return out;
    }
    fresh.busy = true; fresh.fetched_at = time(nullptr); fresh.attempt_at = out.value.attempt_at;
    summary.reset(); out.value = std::move(fresh); out.summary_ok = true;
    struct Endpoint { std::string path; bool (*parse)(const cJSON*, paperdesk_garmin::Snapshot&); size_t limit; };
    const Endpoint endpoints[] = {
        {"/wellness-service/wellness/dailySleepData/" + display_path + "?date=" + date + "&nonSleepBufferMinutes=60", parse_sleep, MaxHealthResponse},
        {std::string("/wellness-service/wellness/bodyBattery/reports/daily?startDate=") + date + "&endDate=" + date, parse_battery, MaxHealthResponse},
        {"/activitylist-service/activities/search/activities?start=0&limit=1", parse_activity, MaxResponse},
        {std::string("/hrv-service/hrv/") + date, parse_hrv, MaxHealthResponse}
    };
    bool partial = false;
    for (const auto& endpoint : endpoints) {
        Check check;
        auto json = request(std::string(ApiHost) + endpoint.path, "Bearer " + credentials.access,
                            nullptr, deadline, check, endpoint.limit);
        if (check.status == 401 || check.status == 403) {
            fail("佳明授权失效，请重新授权", true); break;
        }
        if (check.status == 429) { fail(request_failure(check)); break; }
        if (!json || !endpoint.parse(json.get(), out.value)) {
            partial = true;
            if (!json && check.status != 204 && check.status != 404) {
                out.failed = true; out.value.status = request_failure(check);
            }
        }
    }
    if (!out.reauth && !out.failed) out.value.status = partial ? "部分指标待同步 · 保留可用数据" : "已直连佳明更新";
    else if (!out.reauth && out.value.status.empty()) out.value.status = "部分更新失败，30分钟后重试";
    // Mark mixed-age or incomplete data so the UI does not imply every field is current.
    out.value.cached = out.value.cached || partial || out.failed;
    return out;
}

void service_task(void*) {
    for (;;) {
        bool connected;
        { std::lock_guard<std::mutex> lock(app_mutex); connected = app.connected; }
        bool run = false;
        Credentials credentials;
        paperdesk_garmin::Snapshot previous;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            int64_t now = esp_timer_get_time();
            bool eligible = connected && time(nullptr) >= 1704067200 && !service.reauth &&
                !state.busy && !service.busy && !state.credentials.refresh.empty() &&
                !(state.refreshed && !state.saved && !service.persist_pending);
            bool due = now >= service.next_due || (service.requested && !service.backoff);
            if (eligible && due) {
                service.requested = false; service.busy = true; service.last_request = now;
                service.current.busy = true; service.current.configured = true;
                service.current.status = "正在直连佳明同步";
                service.current.attempt_at = time(nullptr);
                previous = service.current; credentials = state.credentials; run = true;
            } else if (!connected && !service.busy && service.current.configured && !service.reauth) {
                service.current.cached = service.current.ready;
                service.current.status = service.current.ready ? "离线 · 显示保存的简报" : "连接 Wi-Fi 后同步佳明";
            }
        }
        if (run) {
            dirty = true;
            SyncResult result = synchronize(std::move(credentials), previous);
            result.value.busy = false;
            if (result.summary_ok && !save_summary(result.value) && !result.failed && !result.reauth) {
                result.value.status = "简报已更新 · 本次未能保存";
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                service.current = std::move(result.value); service.busy = false;
                service.reauth = result.reauth; service.backoff = result.failed;
                service.next_due = esp_timer_get_time() + SyncIntervalUs;
            }
            dirty = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void task(void*) {
  {
    Credentials credentials;
    { std::lock_guard<std::mutex> lock(state_mutex); credentials = state.credentials; }
    auto run = [&]() -> const char* {
        time_t now = time(nullptr);
        if (now < 1704067200) return "clock_not_ready";
        tm local{};
        localtime_r(&now, &local);
        char date[16];
        if (!strftime(date, sizeof date, "%Y-%m-%d", &local)) return "date_failed";
        int64_t deadline = esp_timer_get_time() + RunTimeoutUs;
        phase("profile");
        Check profile_check;
        auto profile = request(std::string(ApiHost) + "/userprofile-service/socialProfile",
                               "Bearer " + credentials.access, nullptr, deadline, profile_check);
        auto display = cJSON_GetObjectItemCaseSensitive(profile.get(), "displayName");
        profile_check.valid = cJSON_IsString(display) && display->valuestring[0] &&
                              std::strlen(display->valuestring) <= 256;
        record(0, profile_check);
        if (!profile_check.valid) return "profile_failed";
        std::string path = std::string(ApiHost) + "/usersummary-service/usersummary/daily/" +
                           encode(display->valuestring) + "?calendarDate=" + date;
        profile.reset();
        phase("summary_before_refresh");
        Check before_check;
        auto before = request(path, "Bearer " + credentials.access, nullptr, deadline, before_check);
        std::array<Metric, 8> before_metrics{};
        before_check.valid = summary_valid(before.get(), date, before_metrics);
        record(1, before_check);
        if (!before_check.valid) return "summary_before_failed";
        before.reset();
        phase("refresh");
        std::string form = "grant_type=refresh_token&client_id=" + encode(credentials.client) +
                           "&refresh_token=" + encode(credentials.refresh);
        Check refresh_check;
        auto refreshed = request(TokenUrl, basic_auth(credentials.client), &form, deadline, refresh_check);
        std::fill(form.begin(), form.end(), '\0');
        auto access = cJSON_GetObjectItemCaseSensitive(refreshed.get(), "access_token");
        auto refresh = cJSON_GetObjectItemCaseSensitive(refreshed.get(), "refresh_token");
        refresh_check.valid = cJSON_IsString(access) && ascii_value(access->valuestring, 8192) &&
            (!refresh || (cJSON_IsString(refresh) && ascii_value(refresh->valuestring, 4096)));
        record(2, refresh_check);
        if (!refresh_check.valid) return "refresh_failed";
        credentials.access = access->valuestring;
        if (refresh) credentials.refresh = refresh->valuestring;
        // Retain rotated credentials immediately, even if the following data read fails.
        { std::lock_guard<std::mutex> lock(state_mutex);
          state.credentials = credentials; state.refreshed = true; state.saved = false; }
        refreshed.reset();
        phase("summary_after_refresh");
        Check after_check;
        auto after = request(path, "Bearer " + credentials.access, nullptr, deadline, after_check);
        std::array<Metric, 8> metrics{};
        after_check.valid = summary_valid(after.get(), date, metrics);
        record(3, after_check);
        if (!after_check.valid) return "summary_after_failed";
        { std::lock_guard<std::mutex> lock(state_mutex); state.metrics = metrics; }
        return nullptr;
    };
    const char* error = run();
    finish(error == nullptr, error ? error : "");
    // No application locks survive task deletion; local objects are released first.
  }
    vTaskDelete(nullptr);
}
} // namespace

bool garmin_probe_begin(const char* input, size_t length) {
    if (!input || !length || length > MaxInput || std::memchr(input, '\0', length)) return false;
    std::string body(input, length);
    const char* end = nullptr;
    Json json(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true), cJSON_Delete);
    if (!cJSON_IsObject(json.get())) return false;
    Credentials credentials;
    bool resumed = false;
    auto action = cJSON_GetObjectItemCaseSensitive(json.get(), "action");
    if (action) {
        if (!cJSON_IsString(action) || std::strcmp(action->valuestring, "resume")) return false;
        resumed = true;
        if (!load_credentials(credentials)) return false;
    } else if (!credentials_from_json(json.get(), credentials)) return false;
    std::fill(body.begin(), body.end(), '\0');
    std::lock_guard<std::mutex> lock(state_mutex);
    // Never overwrite an unsaved refresh result: refresh tokens may rotate.
    if (state.busy || service.busy || (state.refreshed && !state.saved)) return false;
    state = {};
    state.credentials = std::move(credentials);
    state.busy = true;
    state.resumed = resumed;
    state.saved = resumed;
    state.phase = "queued";
    if (xTaskCreate(task, "garmin_probe", 12288, nullptr, 2, nullptr) != pdPASS) {
        state.busy = false; state.phase = "failed"; state.error = "task_memory";
        return false;
    }
    return true;
}

char* garmin_probe_result_json() {
    std::lock_guard<std::mutex> lock(state_mutex);
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json) return nullptr;
    cJSON_AddBoolToObject(json.get(), "busy", state.busy);
    cJSON_AddStringToObject(json.get(), "phase", state.phase);
    cJSON_AddBoolToObject(json.get(), "core_passed", state.core_passed);
    cJSON_AddBoolToObject(json.get(), "refreshed", state.refreshed);
    cJSON_AddBoolToObject(json.get(), "saved", state.saved);
    cJSON_AddBoolToObject(json.get(), "resumed", state.resumed);
    cJSON_AddBoolToObject(json.get(), "can_save", !state.busy && !service.busy && state.refreshed);
    cJSON_AddStringToObject(json.get(), "error", state.error);
    cJSON_AddNumberToObject(json.get(), "updated_at", double(state.updated_at));
    auto memory = cJSON_AddObjectToObject(json.get(), "memory");
    cJSON_AddNumberToObject(memory, "internal_free", double(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(memory, "internal_min_free", double(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(memory, "internal_largest_block", double(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(memory, "psram_free", double(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    auto checks = cJSON_AddObjectToObject(json.get(), "checks");
    const char* names[] = {"profile", "summary_before_refresh", "refresh", "summary_after_refresh"};
    for (size_t i = 0; i < state.checks.size(); ++i) {
        auto item = cJSON_AddObjectToObject(checks, names[i]);
        cJSON_AddNumberToObject(item, "status", state.checks[i].status);
        cJSON_AddNumberToObject(item, "bytes", double(state.checks[i].bytes));
        cJSON_AddBoolToObject(item, "complete", state.checks[i].complete);
        cJSON_AddBoolToObject(item, "valid", state.checks[i].valid);
    }
    auto summary = cJSON_AddObjectToObject(json.get(), "summary");
    for (const auto& metric : state.metrics)
        if (metric.present) cJSON_AddNumberToObject(summary, metric.name, metric.value);
    return cJSON_PrintUnformatted(json.get());
}

bool garmin_probe_save() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state.busy || service.busy || !state.refreshed) return false;
    bool ok = save_credentials(state.credentials);
    state.saved = ok;
    if (ok) {
        service.current.configured = true; service.reauth = false;
        service.persist_pending = false; service.backoff = false; service.next_due = 0;
    }
    return ok;
}

namespace paperdesk_garmin {
void start() {
    Credentials credentials;
    bool configured = load_credentials(credentials);
    Snapshot cached;
    load_summary(cached);
    std::lock_guard<std::mutex> lock(state_mutex);
    if (service.started) return;
    service.started = true;
    service.current = std::move(cached);
    if (!state.busy && state.credentials.refresh.empty() && configured) {
        state.credentials = std::move(credentials); state.saved = true;
    }
    service.current.configured = !state.credentials.refresh.empty();
    if (!service.current.configured) service.current.status = "尚未配置佳明授权";
    else if (!service.current.ready) service.current.status = "等待直连佳明同步";
    if (xTaskCreate(service_task, "garmin", 16384, nullptr, 1, nullptr) != pdPASS) {
        service.started = false; service.current.status = "佳明同步启动失败";
    }
}

void request_sync() {
    std::lock_guard<std::mutex> lock(state_mutex);
    int64_t now = esp_timer_get_time();
    if (service.busy || state.busy || !service.current.configured || service.reauth ||
        (service.backoff && now < service.next_due) ||
        (service.last_request && now - service.last_request < 60000000)) return;
    service.last_request = now; service.requested = true;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return service.current;
}

char* status_json() {
    auto value = snapshot();
    auto json = snapshot_json(value);
    if (!json) return nullptr;
    { std::lock_guard<std::mutex> lock(state_mutex);
      cJSON_AddBoolToObject(json.get(), "needs_reauthorization", service.reauth);
      cJSON_AddBoolToObject(json.get(), "tokens_saved", state.saved);
      cJSON_AddNumberToObject(json.get(), "retry_after_seconds", double(std::max(int64_t(0), service.next_due - esp_timer_get_time()) / 1000000)); }
    return cJSON_PrintUnformatted(json.get());
}
} // namespace paperdesk_garmin
