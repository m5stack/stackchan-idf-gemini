// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "first_run.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <M5Unified.h>

#include <config_service/config_store.hpp>
#include <config_service/product.hpp>
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
#include <conversation/conversation_service.hpp>
#include <conversation/gemini_live_client.hpp>
#endif
#include <wifi_config_service/wifi_config_service.hpp>

#include "wifi_sta.hpp"

namespace stackchan::app::first_run {
namespace {

constexpr const char* kTag = "first-run";
constexpr std::size_t kMaxHttpBody = 4096;
constexpr std::uint32_t kGeminiTestTimeoutMs = 15000;

extern "C" const char first_run_setup_html_start[] asm("_binary_first_run_setup_html_start");
extern "C" const char first_run_setup_html_end[] asm("_binary_first_run_setup_html_end");

struct ScanNetwork {
    std::string ssid;
    int rssi = 0;
    bool secure = true;
};

struct State {
    bool missing_wifi = false;
    bool missing_key = false;
    bool setup_started = false;
    bool registered = false;
    bool complete = false;
    bool finish_pending = false;

    std::string scan_state = "idle"; // idle | scanning | done | error
    std::string scan_error;
    std::vector<ScanNetwork> networks;

    std::string wifi_state = "idle"; // idle | connecting | connected | error
    std::string wifi_error;
    std::string wifi_ssid;
    std::string wifi_password;
    std::string wifi_static_ip;
    std::string wifi_gateway;
    std::string wifi_dns;
    std::string wifi_ip;

    std::string gemini_state = "idle"; // idle | testing | success | error
    std::string gemini_error;
    std::string gemini_key;
    std::string gemini_model;
    std::string gemini_voice;
    std::string gemini_prompt;
    std::string tested_signature;
    std::string tested_model;
    std::string tested_voice;
};

struct WifiJob {
    std::string ssid;
    std::string password;
    std::string static_ip;
    std::string gateway;
    std::string dns;
};

struct GeminiJob {
    std::string key;
    std::string model;
    std::string voice;
    std::string prompt;
    std::string signature;
};

#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
struct GeminiSignals {
    std::atomic<bool> error{false};
};
#endif

SemaphoreHandle_t g_lock = nullptr;
State g_state;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_ui_prompt{false};
std::atomic<bool> g_ui_success{false};
std::atomic<bool> g_button_hold_suppress{false};
std::atomic<bool> g_busy{false};
std::atomic<bool> g_resolution_notified{false};
bool g_button_only = false;
std::function<void()> g_ready_callback;

class LockGuard {
public:
    explicit LockGuard(SemaphoreHandle_t lock) : lock_{lock}
    {
        if (lock_ != nullptr) xSemaphoreTake(lock_, portMAX_DELAY);
    }
    ~LockGuard()
    {
        if (lock_ != nullptr) xSemaphoreGive(lock_);
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SemaphoreHandle_t lock_;
};

void ensure_lock()
{
    if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
}

std::string signature_for(const std::string& key, const std::string& model,
                          const std::string& voice, const std::string& prompt)
{
    std::string out;
    out.reserve(key.size() + model.size() + voice.size() + prompt.size() + 4);
    out += key;
    out.push_back('\n');
    out += model;
    out.push_back('\n');
    out += voice;
    out.push_back('\n');
    out += prompt;
    return out;
}

std::string json_escape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

void send_json(httpd_req_t* req, int status, const std::string& body)
{
    char status_text[40];
    std::snprintf(status_text, sizeof(status_text), "%d %s", status,
                  status == 202 ? "Accepted"
                  : status == 200 ? "OK"
                  : status == 400 ? "Bad Request"
                  : status == 404 ? "Not Found"
                  : status == 409 ? "Conflict"
                  : status == 500 ? "Internal Server Error"
                                  : "Error");
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    (void)httpd_resp_send(req, body.data(), body.size());
}

void send_accepted(httpd_req_t* req)
{
    send_json(req, 202, R"({"accepted":true})");
}

bool read_json(httpd_req_t* req, cJSON** root)
{
    *root = nullptr;
    const int len = req->content_len;
    if (len < 0 || static_cast<std::size_t>(len) > kMaxHttpBody) {
        send_json(req, 400, R"({"error":"request body is too large"})");
        return false;
    }
    if (len == 0) {
        send_json(req, 400, R"({"error":"JSON body required"})");
        return false;
    }
    std::string body(static_cast<std::size_t>(len), '\0');
    std::size_t off = 0;
    while (off < body.size()) {
        const int got = httpd_req_recv(req, body.data() + off, body.size() - off);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            send_json(req, 400, R"({"error":"failed to read request body"})");
            return false;
        }
        off += static_cast<std::size_t>(got);
    }
    *root = cJSON_ParseWithLength(body.data(), body.size());
    if (*root == nullptr || !cJSON_IsObject(*root)) {
        cJSON_Delete(*root);
        *root = nullptr;
        send_json(req, 400, R"({"error":"invalid JSON object"})");
        return false;
    }
    return true;
}

bool setup_routes_ready()
{
    bool setup_started;
    {
        LockGuard lock{g_lock};
        setup_started = g_state.setup_started;
    }
    return setup_started && wifi_ap_active() && wifi_ap_first_run();
}

bool require_setup(httpd_req_t* req)
{
    if (!setup_routes_ready()) {
        send_json(req, 409, R"({"error":"first-run setup is not active"})");
        return false;
    }
    return true;
}

bool valid_ipv4(const std::string& text)
{
    if (text.empty()) return false;
    esp_ip4_addr_t parsed{};
    return esp_netif_str_to_ip4(text.c_str(), &parsed) == ESP_OK && parsed.addr != 0;
}

bool valid_static_fields(const std::string& ip, const std::string& gateway,
                         const std::string& dns, std::string* error)
{
    if (ip.empty()) {
        if (!gateway.empty() || !dns.empty()) {
            *error = "Clear gateway and DNS when using DHCP.";
            return false;
        }
        return true;
    }
    if (!valid_ipv4(ip)) {
        *error = "Static IP must be a valid IPv4 address.";
        return false;
    }
    if (!valid_ipv4(gateway)) {
        *error = "Gateway is required for a static IP.";
        return false;
    }
    if (!dns.empty() && !valid_ipv4(dns)) {
        *error = "DNS must be a valid IPv4 address or empty.";
        return false;
    }
    return true;
}

bool copy_string_field(const cJSON* root, const char* key, std::size_t max_len,
                       std::string* out, std::string* error)
{
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == nullptr || cJSON_IsNull(item)) {
        out->clear();
        return true;
    }
    if (!cJSON_IsString(item)) {
        *error = std::string(key) + " must be a string.";
        return false;
    }
    const std::size_t len = std::strlen(item->valuestring);
    if (len > max_len) {
        *error = std::string(key) + " is too long.";
        return false;
    }
    *out = item->valuestring;
    return true;
}

void notify_resolved_once()
{
    std::function<void()> callback;
    {
        LockGuard lock{g_lock};
        if (!g_resolution_notified.exchange(true)) callback = g_ready_callback;
    }
    if (callback) callback();
}

void begin_skip()
{
    if (!g_enabled.load()) return;
    g_ui_prompt.store(false);
    g_ui_success.store(false);
    notify_resolved_once();
}

void begin_setup()
{
    if (!g_enabled.load() || !g_ui_prompt.load()) return;
    g_ui_prompt.store(false);
    {
        LockGuard lock{g_lock};
        g_state.setup_started = true;
        g_state.wifi_state = "idle";
        g_state.gemini_state = "idle";
        g_state.scan_state = "idle";
    }
    wifi_config::set_first_run_mode(true);
    wifi_set_setup_mode(true);
    wifi_enable_ap_mode(true);
    if (!wifi_ap_active() || !wifi_ap_first_run()) {
        // Return to the prompt instead of stranding the user on a dead setup
        // screen. Undo every piece of setup state that may have been set by a
        // partial AP bring-up.
        if (wifi_ap_active()) (void)wifi_disable_ap_mode();
        wifi_config::set_first_run_mode(false);
        wifi_set_setup_mode(false);
        {
            LockGuard lock{g_lock};
            g_state.setup_started = false;
            g_state.wifi_state = "idle";
            g_state.gemini_state = "idle";
            g_state.scan_state = "idle";
        }
        g_ui_prompt.store(true);
        ESP_LOGE(kTag, "SoftAP could not be started");
    }
}

std::string status_json()
{
    State snap;
    {
        LockGuard lock{g_lock};
        snap = g_state;
    }
    std::string body = "{";
    body += "\"setup_active\":" + std::string(setup_routes_ready() ? "true" : "false") + ",";
    body += "\"wifi_state\":\"" + json_escape(snap.wifi_state) + "\",";
    body += "\"wifi_error\":\"" + json_escape(snap.wifi_error) + "\",";
    body += "\"wifi_connected\":" +
            std::string((snap.wifi_state == "connected" && wifi_is_connected()) ? "true" : "false") + ",";
    body += "\"wifi_ssid\":\"" + json_escape(snap.wifi_ssid) + "\",";
    body += "\"wifi_ip\":\"" + json_escape(wifi_ip_address()) + "\",";
    body += "\"static_ip\":\"" + json_escape(snap.wifi_static_ip) + "\",";
    body += "\"gateway\":\"" + json_escape(snap.wifi_gateway) + "\",";
    body += "\"dns\":\"" + json_escape(snap.wifi_dns) + "\",";
    body += "\"scan_state\":\"" + json_escape(snap.scan_state) + "\",";
    body += "\"scan_error\":\"" + json_escape(snap.scan_error) + "\",";
    body += "\"networks\":[";
    for (std::size_t i = 0; i < snap.networks.size(); ++i) {
        if (i != 0) body += ",";
        const auto& n = snap.networks[i];
        body += "{\"ssid\":\"" + json_escape(n.ssid) + "\",\"rssi\":" +
                std::to_string(n.rssi) + ",\"secure\":" +
                (n.secure ? "true" : "false") + "}";
    }
    body += "],";
    body += "\"gemini_state\":\"" + json_escape(snap.gemini_state) + "\",";
    body += "\"gemini_error\":\"" + json_escape(snap.gemini_error) + "\",";
    body += "\"system_prompt\":\"" + json_escape(snap.gemini_prompt) + "\",";
    body += "\"gemini_test_ok\":" +
            std::string((snap.gemini_state == "success" && !snap.tested_signature.empty())
                            ? "true" : "false") + ",";
    body += "\"tested_model\":\"" + json_escape(snap.tested_model) + "\",";
    body += "\"tested_voice\":\"" + json_escape(snap.tested_voice) + "\",";
    body += "\"default_model\":\"" + json_escape(config::default_gemini_model()) + "\",";
    body += "\"default_voice\":\"" + json_escape(config::default_gemini_voice()) + "\",";
    body += "\"can_finish\":" +
            std::string((snap.wifi_state == "connected" && snap.gemini_state == "success" &&
                         !snap.tested_signature.empty()) ? "true" : "false") + ",";
    body += "\"finish_pending\":" + std::string(snap.finish_pending ? "true" : "false") + ",";
    body += "\"complete\":" + std::string(snap.complete ? "true" : "false");
    body += "}";
    return body;
}

esp_err_t handle_setup_get(httpd_req_t* req)
{
    if (!g_enabled.load() || !setup_routes_ready()) {
        send_json(req, 404, R"({"error":"setup page is only available in first-run mode"})");
        return ESP_OK;
    }
    const std::size_t len =
        static_cast<std::size_t>(first_run_setup_html_end - first_run_setup_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, first_run_setup_html_start, len);
}

esp_err_t handle_status_get(httpd_req_t* req)
{
    if (!g_enabled.load() || !setup_routes_ready()) {
        send_json(req, 404, R"({"error":"first-run setup is not active"})");
        return ESP_OK;
    }
    send_json(req, 200, status_json());
    return ESP_OK;
}

void scan_task(void*)
{
    auto results = wifi_scan();
    {
        LockGuard lock{g_lock};
        g_state.networks.clear();
        g_state.networks.reserve(results.size());
        for (const auto& result : results) {
            g_state.networks.push_back(ScanNetwork{result.ssid, result.rssi, result.secure});
        }
        g_state.scan_state = "done";
        g_state.scan_error.clear();
    }
    g_busy.store(false);
    vTaskDelete(nullptr);
}

esp_err_t handle_scan_post(httpd_req_t* req)
{
    if (!require_setup(req)) return ESP_OK;
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        send_json(req, 409, R"({"error":"another setup operation is in progress"})");
        return ESP_OK;
    }
    {
        LockGuard lock{g_lock};
        g_state.scan_state = "scanning";
        g_state.scan_error.clear();
    }
    if (xTaskCreate(scan_task, "firstrun-scan", 4096, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        g_busy.store(false);
        {
            LockGuard lock{g_lock};
            g_state.scan_state = "error";
            g_state.scan_error = "Could not start Wi-Fi scan.";
        }
        send_json(req, 500, R"({"error":"failed to start Wi-Fi scan"})");
        return ESP_OK;
    }
    send_accepted(req);
    return ESP_OK;
}

void wifi_connect_task(void* arg)
{
    std::unique_ptr<WifiJob> job{static_cast<WifiJob*>(arg)};
    const bool ok = wifi_connect_temporary(job->ssid, job->password, job->static_ip,
                                           job->gateway, job->dns);
    const std::string ip = ok ? wifi_ip_address() : std::string{};
    {
        LockGuard lock{g_lock};
        if (ok && wifi_is_connected() && !ip.empty()) {
            g_state.wifi_state = "connected";
            g_state.wifi_error.clear();
            g_state.wifi_ssid = job->ssid;
            g_state.wifi_password = job->password;
            g_state.wifi_static_ip = job->static_ip;
            g_state.wifi_gateway = job->gateway;
            g_state.wifi_dns = job->dns;
            g_state.wifi_ip = ip;
            // A new network invalidates the previous Gemini session test.
            g_state.gemini_state = "idle";
            g_state.gemini_error.clear();
            g_state.tested_signature.clear();
            g_state.tested_model.clear();
            g_state.tested_voice.clear();
        } else {
            g_state.wifi_state = "error";
            g_state.wifi_error = ok ? "Connected, but no IPv4 address was assigned."
                                    : "Could not connect. Check the SSID/password and network settings.";
            g_state.wifi_ip.clear();
        }
    }
    g_busy.store(false);
    vTaskDelete(nullptr);
}

esp_err_t handle_wifi_post(httpd_req_t* req)
{
    if (!require_setup(req)) return ESP_OK;
    cJSON* root = nullptr;
    if (!read_json(req, &root)) return ESP_OK;

    std::string ssid;
    std::string password;
    std::string static_ip;
    std::string gateway;
    std::string dns;
    std::string error;
    const bool parsed =
        copy_string_field(root, "ssid", 32, &ssid, &error) &&
        copy_string_field(root, "password", 64, &password, &error) &&
        copy_string_field(root, "static_ip", 15, &static_ip, &error) &&
        copy_string_field(root, "gateway", 15, &gateway, &error) &&
        copy_string_field(root, "dns", 15, &dns, &error);
    cJSON_Delete(root);
    if (!parsed) {
        send_json(req, 400, "{\"error\":\"" + json_escape(error) + "\"}");
        return ESP_OK;
    }
    if (ssid.empty()) {
        send_json(req, 400, R"({"error":"Select a Wi-Fi network first."})");
        return ESP_OK;
    }
    if (!valid_static_fields(static_ip, gateway, dns, &error)) {
        send_json(req, 400, "{\"error\":\"" + json_escape(error) + "\"}");
        return ESP_OK;
    }

    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        send_json(req, 409, R"({"error":"another setup operation is in progress"})");
        return ESP_OK;
    }
    {
        LockGuard lock{g_lock};
        g_state.wifi_state = "connecting";
        g_state.wifi_error.clear();
        g_state.wifi_ip.clear();
        g_state.gemini_state = "idle";
        g_state.tested_signature.clear();
    }
    auto* job = new WifiJob{ssid, password, static_ip, gateway, dns};
    if (xTaskCreate(wifi_connect_task, "firstrun-wifi", 6144, job,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        delete job;
        g_busy.store(false);
        LockGuard lock{g_lock};
        g_state.wifi_state = "error";
        g_state.wifi_error = "Could not start Wi-Fi connection.";
        send_json(req, 500, R"({"error":"failed to start Wi-Fi connection"})");
        return ESP_OK;
    }
    send_accepted(req);
    return ESP_OK;
}

#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
void gemini_test_task(void* arg)
{
    std::unique_ptr<GeminiJob> job{static_cast<GeminiJob*>(arg)};
    auto signals = std::make_shared<GeminiSignals>();

    stackchan::conversation::GeminiLiveClient client{job->key};
    client.set_event_callback([signals](const stackchan::conversation::ConversationEvent& event) {
        if (event.type == stackchan::conversation::ConversationEventType::Error ||
            (event.type == stackchan::conversation::ConversationEventType::StateChanged &&
             event.state == stackchan::conversation::ConversationState::Error)) {
            signals->error.store(true, std::memory_order_release);
        }
    });

    stackchan::conversation::ConversationConfig config{};
    config.model = job->model;
    config.voice = job->voice;
    config.instructions = job->prompt.empty()
        ? "You are Stack-chan. Reply briefly. Do not call tools during this setup test."
        : job->prompt;
    config.input_sample_rate_hz = 16000;
    config.output_sample_rate_hz = 24000;
    config.enable_input_transcription = false;

    bool success = false;
    std::string error;
    auto started = client.start(config);
    if (!started) {
        error = "The WebSocket connection could not be created.";
    } else {
        const std::int64_t deadline = esp_timer_get_time() + kGeminiTestTimeoutMs * 1000;
        while (esp_timer_get_time() < deadline) {
            const auto state = client.state();
            if (state == stackchan::conversation::ConversationState::Listening ||
                state == stackchan::conversation::ConversationState::Speaking) {
                success = true;
                break;
            }
            if (signals->error.load(std::memory_order_acquire) ||
                state == stackchan::conversation::ConversationState::Error) {
                error = "Gemini rejected the connection or session setup.";
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!success && error.empty()) {
            error = "Timed out waiting for Gemini setupComplete.";
        }
    }
    client.stop();

    {
        LockGuard lock{g_lock};
        if (success) {
            g_state.gemini_state = "success";
            g_state.gemini_error.clear();
            g_state.gemini_key = job->key;
            g_state.gemini_model = job->model;
            g_state.gemini_voice = job->voice;
            g_state.gemini_prompt = job->prompt;
            g_state.tested_signature = job->signature;
            g_state.tested_model = job->model;
            g_state.tested_voice = job->voice;
        } else {
            g_state.gemini_state = "error";
            g_state.gemini_error = error;
            g_state.tested_signature.clear();
            g_state.tested_model.clear();
            g_state.tested_voice.clear();
        }
    }
    g_busy.store(false);
    ESP_LOGI(kTag, "Gemini connection test %s", success ? "passed" : "failed");
    vTaskDelete(nullptr);
}
#endif

esp_err_t handle_gemini_test_post(httpd_req_t* req)
{
    if (!require_setup(req)) return ESP_OK;
#if !CONFIG_STACKCHAN_CONVERSATION_ENABLED
    send_json(req, 503,
              R"({"error":"Gemini testing is unavailable in this conversation-disabled build."})");
    return ESP_OK;
#else
    State snap;
    {
        LockGuard lock{g_lock};
        if (g_state.finish_pending) {
            send_json(req, 409, R"({"error":"setup is already finishing."})");
            return ESP_OK;
        }
        snap = g_state;
    }
    if (snap.wifi_state != "connected" || !wifi_is_connected()) {
        send_json(req, 409, R"({"error":"Connect Wi-Fi successfully before testing Gemini."})");
        return ESP_OK;
    }

    cJSON* root = nullptr;
    if (!read_json(req, &root)) return ESP_OK;
    std::string key;
    std::string model;
    std::string voice;
    std::string prompt;
    std::string error;
    const bool parsed =
        copy_string_field(root, "api_key", 256, &key, &error) &&
        copy_string_field(root, "model", 64, &model, &error) &&
        copy_string_field(root, "voice", 32, &voice, &error) &&
        copy_string_field(root, "system_prompt", 2048, &prompt, &error);
    cJSON_Delete(root);
    if (!parsed) {
        send_json(req, 400, "{\"error\":\"" + json_escape(error) + "\"}");
        return ESP_OK;
    }
    if (key.empty()) {
        send_json(req, 400, R"({"error":"Gemini API key is required."})");
        return ESP_OK;
    }
    if (model.empty()) model = config::default_gemini_model();
    if (voice.empty()) voice = config::default_gemini_voice();

    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        send_json(req, 409, R"({"error":"another setup operation is in progress"})");
        return ESP_OK;
    }
    {
        LockGuard lock{g_lock};
        g_state.gemini_state = "testing";
        g_state.gemini_error.clear();
        g_state.tested_signature.clear();
        g_state.tested_model.clear();
        g_state.tested_voice.clear();
    }
    auto* job = new GeminiJob{key, model, voice, prompt,
                              signature_for(key, model, voice, prompt)};
    if (xTaskCreate(gemini_test_task, "firstrun-gemini", 10240, job,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        delete job;
        g_busy.store(false);
        LockGuard lock{g_lock};
        g_state.gemini_state = "error";
        g_state.gemini_error = "Could not start the Gemini connection test.";
        send_json(req, 500, R"({"error":"failed to start Gemini connection test"})");
        return ESP_OK;
    }
    send_accepted(req);
    return ESP_OK;
#endif
}

void restart_after_finish()
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(kTag, "restarting into configured conversation mode");
    esp_restart();
}

void finish_task(void*)
{
    State snap;
    {
        LockGuard lock{g_lock};
        snap = g_state;
    }
    config::DeviceConfig cfg = config::load();
    cfg.wifi_ssid = snap.wifi_ssid;
    cfg.wifi_password = snap.wifi_password;
    cfg.wifi_static_ip = snap.wifi_static_ip;
    cfg.wifi_gateway = snap.wifi_gateway;
    cfg.wifi_dns = snap.wifi_dns;
    cfg.gemini_api_key = snap.gemini_key;
    cfg.gemini_model = snap.gemini_model;
    cfg.gemini_voice = snap.gemini_voice;
    cfg.system_prompt = snap.gemini_prompt;
    cfg.provider = config::Provider::Gemini;
    cfg.openai_enabled = true;
    cfg.jtts_idle_enabled = false;
    cfg.operation_mode = config::OperationMode::Conversation;

    const auto saved = config::store::save(cfg);
    if (!saved) {
        {
            LockGuard lock{g_lock};
            g_state.finish_pending = false;
            g_state.gemini_state = "error";
            g_state.gemini_error = "Could not save setup to NVS. Please retry.";
        }
        g_busy.store(false);
        ESP_LOGE(kTag, "failed to save first-run configuration");
        vTaskDelete(nullptr);
        return;
    }

    {
        LockGuard lock{g_lock};
        g_state.finish_pending = false;
        g_state.complete = true;
    }
    g_ui_success.store(true);
    // Keep the setup AP and status route alive briefly so the browser can
    // observe complete=true instead of inferring success from a lost socket.
    vTaskDelay(pdMS_TO_TICKS(1200));
    wifi_config::set_first_run_mode(false);
    const bool ap_down = wifi_disable_ap_mode();
    g_busy.store(false);
    ESP_LOGI(kTag, "setup complete; AP down=%d", static_cast<int>(ap_down));
    restart_after_finish();
    vTaskDelete(nullptr);
}

esp_err_t handle_finish_post(httpd_req_t* req)
{
    if (!require_setup(req)) return ESP_OK;

    State snap;
    {
        LockGuard lock{g_lock};
        if (g_state.finish_pending) {
            send_json(req, 409, R"({"error":"setup is already finishing."})");
            return ESP_OK;
        }
        snap = g_state;
    }
    if (snap.wifi_state != "connected" || !wifi_is_connected()) {
        send_json(req, 409, R"({"error":"Wi-Fi is not connected."})");
        return ESP_OK;
    }
    if (snap.gemini_state != "success" || snap.tested_signature.empty()) {
        send_json(req, 409, R"({"error":"Test the Gemini connection successfully before finishing."})");
        return ESP_OK;
    }

    // If the page sends the fields again, require them to match the exact
    // values covered by the successful test. An omitted/blank API key reuses
    // the in-memory tested key so a page reload can still finish.
    if (req->content_len > 0) {
        cJSON* root = nullptr;
        if (!read_json(req, &root)) return ESP_OK;
        std::string key;
        std::string model;
        std::string voice;
        std::string prompt;
        std::string error;
        const bool parsed =
            copy_string_field(root, "api_key", 256, &key, &error) &&
            copy_string_field(root, "model", 64, &model, &error) &&
            copy_string_field(root, "voice", 32, &voice, &error) &&
            copy_string_field(root, "system_prompt", 2048, &prompt, &error);
        cJSON_Delete(root);
        if (!parsed) {
            send_json(req, 400, "{\"error\":\"" + json_escape(error) + "\"}");
            return ESP_OK;
        }
        if (key.empty()) key = snap.gemini_key;
        if (model.empty()) model = snap.gemini_model;
        if (voice.empty()) voice = snap.gemini_voice;
        if (signature_for(key, model, voice, prompt) != snap.tested_signature) {
            send_json(req, 409,
                      R"({"error":"Chat settings changed after the successful test. Test again."})");
            return ESP_OK;
        }
    }

    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        send_json(req, 409, R"({"error":"another setup operation is in progress"})");
        return ESP_OK;
    }
    {
        LockGuard lock{g_lock};
        g_state.finish_pending = true;
    }
    if (xTaskCreate(finish_task, "firstrun-finish", 6144, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        g_busy.store(false);
        LockGuard lock{g_lock};
        g_state.finish_pending = false;
        send_json(req, 500, R"({"error":"failed to finish setup"})");
        return ESP_OK;
    }
    send_accepted(req);
    return ESP_OK;
}

void register_routes(httpd_handle_t server)
{
    httpd_uri_t routes[] = {
        {.uri = "/setup", .method = HTTP_GET, .handler = handle_setup_get, .user_ctx = nullptr},
        {.uri = "/api/first-run/status", .method = HTTP_GET, .handler = handle_status_get,
         .user_ctx = nullptr},
        {.uri = "/api/first-run/scan", .method = HTTP_POST, .handler = handle_scan_post,
         .user_ctx = nullptr},
        {.uri = "/api/first-run/wifi", .method = HTTP_POST, .handler = handle_wifi_post,
         .user_ctx = nullptr},
        {.uri = "/api/first-run/gemini/test", .method = HTTP_POST,
         .handler = handle_gemini_test_post, .user_ctx = nullptr},
        {.uri = "/api/first-run/finish", .method = HTTP_POST, .handler = handle_finish_post,
         .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        esp_err_t err = httpd_register_uri_handler(server, &route);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "route %s registration failed: %s", route.uri,
                     esp_err_to_name(err));
        }
    }
    LockGuard lock{g_lock};
    g_state.registered = true;
}

void route_watcher_task(void*)
{
    for (;;) {
        bool setup_started;
        bool registered;
        {
            LockGuard lock{g_lock};
            setup_started = g_state.setup_started;
            registered = g_state.registered;
        }
        if (setup_started && !registered) {
            if (httpd_handle_t server = wifi_config::handle(); server != nullptr) {
                register_routes(server);
                ESP_LOGI(kTag, "first-run HTTP routes registered");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

std::int32_t g_start_x = 0;
std::int32_t g_start_y = 0;
std::int32_t g_start_w = 0;
std::int32_t g_start_h = 0;
std::int32_t g_skip_x = 0;
std::int32_t g_skip_y = 0;
std::int32_t g_skip_w = 0;
std::int32_t g_skip_h = 0;

void draw_prompt(avatar::RichCanvas& canvas)
{
    const std::int32_t w = canvas.width();
    const std::int32_t h = canvas.height();
    const bool compact = w < 200;
    const std::uint16_t paper = canvas.color565(245, 248, 251);
    const std::uint16_t ink = canvas.color565(20, 35, 52);
    const std::uint16_t muted = canvas.color565(80, 100, 120);
    const std::uint16_t blue = canvas.color565(8, 122, 193);
    const std::uint16_t grey = canvas.color565(224, 230, 236);

    canvas.begin_frame(paper);
    canvas.setTextColor(ink, paper);
    canvas.setTextDatum(lgfx::textdatum_t::top_left);
    canvas.setFont(&fonts::Font0);
    if (!compact) canvas.setFont(&fonts::lgfxJapanGothic_16);
    canvas.drawString("Stack-chan setup", compact ? 6 : 18, compact ? 8 : 18);

    canvas.setTextColor(muted, paper);
    canvas.setFont(&fonts::Font0);
    if (!compact) canvas.setFont(&fonts::lgfxJapanGothic_12);
    bool missing_wifi;
    bool missing_key;
    {
        LockGuard lock{g_lock};
        missing_wifi = g_state.missing_wifi;
        missing_key = g_state.missing_key;
    }
    if (compact) {
        canvas.drawString(missing_wifi && missing_key ? "Wi-Fi + API key missing"
                          : missing_wifi ? "Wi-Fi is not configured"
                                         : "Gemini API key missing",
                          6, 32);
        canvas.drawString("A: setup   hold A: skip", 6, h - 44);
    } else {
        const char* line1 = missing_wifi && missing_key
                                ? "Wi-Fi and Gemini API key are not configured."
                            : missing_wifi ? "Wi-Fi is not configured."
                                           : "Gemini API key is not configured.";
        canvas.drawString(line1, 18, 66);
        canvas.drawString("Start setup opens a guided phone setup page.", 18, 88);
    }

    const std::int32_t margin = compact ? 6 : 14;
    const std::int32_t gap = compact ? 4 : 10;
    const std::int32_t button_h = compact ? 24 : 38;
    const std::int32_t button_y = h - button_h - (compact ? 7 : 14);
    const std::int32_t total_w = w - margin * 2;
    const std::int32_t start_w = (total_w - gap) * 3 / 5;
    const std::int32_t skip_w = total_w - gap - start_w;
    g_start_x = margin;
    g_start_y = button_y;
    g_start_w = start_w;
    g_start_h = button_h;
    g_skip_x = margin + start_w + gap;
    g_skip_y = button_y;
    g_skip_w = skip_w;
    g_skip_h = button_h;

    canvas.fillRoundRect(g_skip_x, g_skip_y, g_skip_w, g_skip_h, 6, grey);
    canvas.setTextColor(ink, grey);
    canvas.setTextDatum(lgfx::textdatum_t::middle_center);
    canvas.setFont(&fonts::Font0);
    if (!compact) canvas.setFont(&fonts::lgfxJapanGothic_12);
    canvas.drawString("Skip", g_skip_x + g_skip_w / 2, g_skip_y + g_skip_h / 2);

    canvas.fillRoundRect(g_start_x, g_start_y, g_start_w, g_start_h, 6, blue);
    canvas.setTextColor(TFT_WHITE, blue);
    canvas.drawString("Start setup", g_start_x + g_start_w / 2,
                      g_start_y + g_start_h / 2);
    canvas.setTextDatum(lgfx::textdatum_t::top_left);
    return;
}

void draw_success(avatar::RichCanvas& canvas)
{
    const std::int32_t w = canvas.width();
    const std::int32_t h = canvas.height();
    const bool compact = w < 200;
    const std::uint16_t blue = canvas.color565(8, 122, 193);
    canvas.begin_frame(blue);
    canvas.setTextColor(TFT_WHITE, blue);
    canvas.setTextDatum(lgfx::textdatum_t::middle_center);
    canvas.setFont(&fonts::Font0);
    if (!compact) canvas.setFont(&fonts::lgfxJapanGothic_16);
    canvas.drawString("Setup complete", w / 2, h / 2 - 12);
    if (!compact) {
        canvas.setFont(&fonts::lgfxJapanGothic_12);
        canvas.drawString("Restarting...", w / 2, h / 2 + 16);
    }
    canvas.setTextDatum(lgfx::textdatum_t::top_left);
}

bool hit(std::int32_t x, std::int32_t y, std::int32_t bx, std::int32_t by,
         std::int32_t bw, std::int32_t bh)
{
    return bw > 0 && bh > 0 && x >= bx && x < bx + bw && y >= by && y < by + bh;
}

} // namespace

void init(bool required, const config::DeviceConfig& cfg, bool button_only)
{
    ensure_lock();
    g_button_only = button_only;
    constexpr bool conversation_supported =
#if CONFIG_STACKCHAN_CONVERSATION_ENABLED
        true;
#else
        false;
#endif
    const bool enabled = config::gemini_only() && required && conversation_supported;
    g_enabled.store(enabled);
    {
        LockGuard lock{g_lock};
        g_state = State{};
        g_state.missing_wifi = cfg.wifi_ssid.empty();
        g_state.missing_key = cfg.gemini_api_key.empty();
        if (!cfg.gemini_model.empty()) g_state.gemini_model = cfg.gemini_model;
        if (!cfg.gemini_voice.empty()) g_state.gemini_voice = cfg.gemini_voice;
        if (!cfg.system_prompt.empty()) g_state.gemini_prompt = cfg.system_prompt;
    }
    g_ui_prompt.store(enabled);
    g_ui_success.store(false);
    g_button_hold_suppress.store(false);
    g_resolution_notified.store(false);
    if (enabled) {
        ESP_LOGI(kTag, "prompt enabled (wifi_missing=%d key_missing=%d)",
                 static_cast<int>(cfg.wifi_ssid.empty()),
                 static_cast<int>(cfg.gemini_api_key.empty()));
        if (xTaskCreate(route_watcher_task, "firstrun-http", 4096, nullptr,
                        tskIDLE_PRIORITY + 1, nullptr) != pdPASS) {
            ESP_LOGE(kTag, "failed to create HTTP route watcher");
        }
    }
}

void set_ready_callback(std::function<void()> callback)
{
    ensure_lock();
    std::function<void()> notify_now;
    {
        LockGuard lock{g_lock};
        g_ready_callback = std::move(callback);
        // Skip may resolve before app_main installs the continuation. Deliver
        // an already-resolved transition immediately rather than losing it.
        if (g_resolution_notified.load(std::memory_order_acquire)) {
            notify_now = g_ready_callback;
        }
    }
    if (notify_now) notify_now();
}

bool screen_active()
{
    // On button-only boards, a long-press Skip resolves the prompt before the
    // physical button is released. Keep ownership for that remaining hold so
    // the status overlay underneath cannot interpret the same press as its
    // own AP-toggle gesture.
    return g_ui_prompt.load() || g_ui_success.load() ||
           g_button_hold_suppress.load();
}

bool draw(avatar::RichCanvas& canvas)
{
    if (g_ui_success.load()) {
        draw_success(canvas);
        return true;
    }
    if (g_ui_prompt.load()) {
        draw_prompt(canvas);
        return true;
    }
    return false;
}

bool handle_tap(int x, int y)
{
    if (g_ui_success.load()) return true;
    if (!g_ui_prompt.load()) return false;
    if (hit(x, y, g_start_x, g_start_y, g_start_w, g_start_h)) {
        begin_setup();
        return true;
    }
    if (hit(x, y, g_skip_x, g_skip_y, g_skip_w, g_skip_h)) {
        begin_skip();
        return true;
    }
    return true; // prompt owns the panel and must not leak taps underneath
}

void poll_button()
{
    if (!g_button_only) return;
    if (g_button_hold_suppress.load()) {
        if (!M5.BtnA.isPressed()) g_button_hold_suppress.store(false);
        return;
    }
    if (!g_ui_prompt.load()) return;
    if (M5.BtnA.wasHold()) {
        g_button_hold_suppress.store(true);
        begin_skip();
    } else if (M5.BtnA.wasClicked()) {
        begin_setup();
    }
}

bool active()
{
    if (!g_enabled.load()) return false;
    bool setup_started;
    {
        LockGuard lock{g_lock};
        setup_started = g_state.setup_started;
    }
    return g_ui_prompt.load() || g_ui_success.load() || setup_started;
}

} // namespace stackchan::app::first_run
