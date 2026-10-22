// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_sta.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <M5Unified.h>

#include <config_service/config_service.hpp>
#include <wifi_config_service/wifi_config_service.hpp>

#include "captive_portal.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "wifi-sta";

// Bounded copy into a fixed field that always NUL-terminates. Used for the
// esp_wifi AP/STA config's byte arrays. strncpy(dst, src, sizeof-1) and
// snprintf both trip GCC 14's -Werror=stringop-truncation /
// format-truncation under IDF 5.5; an explicit memcpy of a clamped length
// followed by a NUL is accepted on both IDF 5.4 (GCC 13) and 5.5 (GCC 14).
void copy_field(std::uint8_t* dst, std::size_t cap, const char* src)
{
    std::size_t n = std::strlen(src);
    if (n > cap - 1) n = cap - 1;
    std::memcpy(dst, src, n);
    dst[n] = 0;
}

std::atomic<bool> g_connected{false};
std::atomic<bool> g_ap_active{false};
std::atomic<bool> g_ap_first_run{false};
std::atomic<bool> g_setup_mode{false};
std::atomic<bool> g_wifi_inited{false};

// Cached AP credentials so wifi_ap_info() can serve the on-device QR screen
// without re-querying the driver. Derived from ESP_MAC_WIFI_STA.
char g_ap_ssid[33] = {0};
char g_ap_pw[33]   = {0};
char g_ap_ip[16]   = "192.168.4.1";
esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;

// Snapshot of the config used at boot — held so the HTTP settings service
// can be brought up on the first IP_EVENT_STA_GOT_IP with the same view of
// NVS that the BLE service registered. After the first start it stays up
// across reconnects (wifi_config_service::start ignores re-entry).
const config::DeviceConfig* g_boot_cfg = nullptr;
char g_configured_ssid[33] = {0};

// One-shot timer that defers the next esp_wifi_connect() while the
// provisioning AP is up. Each STA connect attempt runs a full-channel scan
// (~5 s) that pulls the shared radio off the AP's channel — with the
// immediate-retry loop the AP beacons go silent most of the time and joined
// clients get kicked within seconds. While the AP is active the retry is
// throttled to this period, and paused entirely while a client is associated.
constexpr std::uint64_t kApModeStaRetryUs = 30 * 1000 * 1000;
esp_timer_handle_t g_sta_retry_timer = nullptr;
std::atomic<int> g_ap_client_count{0};

bool can_auto_connect()
{
    if (g_setup_mode.load(std::memory_order_acquire)) return false;
    if (g_configured_ssid[0] == '\0') return false;
    if (g_ap_active.load(std::memory_order_acquire) &&
        g_ap_client_count.load(std::memory_order_acquire) > 0) {
        return false;
    }
    return true;
}

void stop_retry_timer()
{
    if (g_sta_retry_timer != nullptr) {
        (void)esp_timer_stop(g_sta_retry_timer);
    }
}

void sta_retry_timer_cb(void*)
{
    if (can_auto_connect()) {
        (void)esp_wifi_connect();
    }
}

bool valid_ipv4(const std::string& text, esp_ip4_addr_t* out)
{
    if (text.empty()) return false;
    esp_ip4_addr_t parsed{};
    if (esp_netif_str_to_ip4(text.c_str(), &parsed) != ESP_OK || parsed.addr == 0) {
        return false;
    }
    if (out != nullptr) *out = parsed;
    return true;
}

esp_err_t start_dhcp()
{
    if (g_sta_netif == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_netif_dhcpc_start(g_sta_netif);
    return (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) ? ESP_OK : err;
}

// Apply the optional static IPv4 configuration. Empty static_ip selects
// DHCP. Static configuration uses a /24 mask, requires a gateway, and uses
// the gateway as DNS when dns is empty. Malformed saved settings fall back
// to DHCP rather than leaving the interface unreachable.
bool apply_sta_ipv4(const std::string& static_ip, const std::string& gateway,
                    const std::string& dns)
{
    if (g_sta_netif == nullptr) return false;

    if (static_ip.empty()) {
        stop_retry_timer();
        const esp_err_t err = start_dhcp();
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "DHCP start failed: %s", esp_err_to_name(err));
        }
        return true;
    }

    esp_ip4_addr_t ip{};
    esp_ip4_addr_t gw{};
    if (!valid_ipv4(static_ip, &ip) || !valid_ipv4(gateway, &gw)) {
        ESP_LOGW(kTag, "invalid static IPv4 config (ip=%s gateway=%s) — falling back to DHCP",
                 static_ip.c_str(), gateway.c_str());
        (void)start_dhcp();
        return false;
    }

    esp_ip4_addr_t dns_addr = gw;
    if (!dns.empty() && !valid_ipv4(dns, &dns_addr)) {
        ESP_LOGW(kTag, "invalid static DNS '%s' — using gateway", dns.c_str());
        dns_addr = gw;
    }

    const esp_err_t stop_err = esp_netif_dhcpc_stop(g_sta_netif);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(kTag, "DHCP stop failed: %s — falling back to DHCP", esp_err_to_name(stop_err));
        (void)start_dhcp();
        return false;
    }

    esp_netif_ip_info_t info{};
    info.ip = ip;
    info.gw = gw;
    (void)esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
    esp_err_t err = esp_netif_set_ip_info(g_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set static IPv4 failed: %s — falling back to DHCP", esp_err_to_name(err));
        (void)start_dhcp();
        return false;
    }

    esp_netif_dns_info_t dns_info{};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4 = dns_addr;
    err = esp_netif_set_dns_info(g_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set static DNS failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(kTag, "STA static IPv4 " IPSTR "/24 gateway " IPSTR " dns " IPSTR,
             IP2STR(&ip), IP2STR(&gw), IP2STR(&dns_addr));
    return true;
}

void schedule_retry_if_allowed()
{
    if (g_sta_retry_timer == nullptr || !can_auto_connect()) return;
    stop_retry_timer();
    (void)esp_timer_start_once(g_sta_retry_timer, kApModeStaRetryUs);
}

void event_handler(void* /*arg*/, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (can_auto_connect()) {
                (void)esp_wifi_connect();
            } else {
                ESP_LOGI(kTag, "STA started; automatic connect suppressed (%s)",
                         g_setup_mode.load(std::memory_order_acquire) ? "setup mode"
                                                                      : "no SSID");
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_connected.store(false, std::memory_order_release);
            config::notify_wifi_connected(false);
            wifi_config::notify_wifi_connected(false);

            if (g_setup_mode.load(std::memory_order_acquire)) {
                ESP_LOGW(kTag, "STA disconnected in setup mode; retry paused");
            } else if (g_configured_ssid[0] == '\0') {
                ESP_LOGW(kTag, "STA disconnected; no SSID configured, retry paused");
            } else if (g_ap_active.load(std::memory_order_acquire)) {
                stop_retry_timer();
                if (g_ap_client_count.load(std::memory_order_acquire) > 0) {
                    ESP_LOGW(kTag, "disconnected — AP client associated, STA retry paused");
                } else {
                    ESP_LOGW(kTag, "disconnected — AP mode active, next retry in %llu s",
                             kApModeStaRetryUs / 1000000ULL);
                    (void)esp_timer_start_once(g_sta_retry_timer, kApModeStaRetryUs);
                }
            } else {
                ESP_LOGW(kTag, "disconnected, retrying");
                (void)esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            // A provisioning client joined — give it quiet air: no STA
            // scans until every client leaves.
            g_ap_client_count.fetch_add(1, std::memory_order_acq_rel);
            stop_retry_timer();
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            const int left = g_ap_client_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (left <= 0) {
                g_ap_client_count.store(0, std::memory_order_release);
                if (g_ap_active.load(std::memory_order_acquire) &&
                    !g_connected.load(std::memory_order_acquire)) {
                    schedule_retry_if_allowed();
                }
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(kTag, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_connected.store(true, std::memory_order_release);
        config::notify_wifi_connected(true);

        // Bring up mDNS + HTTP settings server on the first successful STA
        // association. Subsequent reconnects are no-ops inside start().
        if (g_boot_cfg != nullptr) {
            if (auto r = wifi_config::start(*g_boot_cfg); !r) {
                if (r.error() != wifi_config::Error::AlreadyStarted) {
                    ESP_LOGW(kTag, "wifi_config::start failed: %d",
                             static_cast<int>(r.error()));
                }
            }
        }
        wifi_config::notify_wifi_connected(true);

        // SNTP — start once on the first connection. JST is the product's
        // default; the time is mirrored into the battery-backed RTC.
        static bool sntp_started = false;
        if (!sntp_started) {
            sntp_started = true;
            setenv("TZ", "JST-9", 1);
            tzset();
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.start = true;
            esp_netif_sntp_init(&cfg);
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) == ESP_OK) {
                time_t now = 0;
                ::time(&now);
                struct tm tm_now{};
                localtime_r(&now, &tm_now);
                m5::rtc_datetime_t dt{};
                dt.date.year    = tm_now.tm_year + 1900;
                dt.date.month   = tm_now.tm_mon + 1;
                dt.date.date    = tm_now.tm_mday;
                dt.date.weekDay = tm_now.tm_wday;
                dt.time.hours   = tm_now.tm_hour;
                dt.time.minutes = tm_now.tm_min;
                dt.time.seconds = tm_now.tm_sec;
                M5.Rtc.setDateTime(dt);
                ESP_LOGI(kTag, "SNTP synced → RTC set %04d-%02d-%02d %02d:%02d:%02d JST",
                         dt.date.year, dt.date.month, dt.date.date,
                         dt.time.hours, dt.time.minutes, dt.time.seconds);
            } else {
                ESP_LOGW(kTag, "SNTP sync timeout (will keep trying in background)");
            }
        }
    }
}

} // namespace

void wifi_start(const config::DeviceConfig& cfg)
{
    g_boot_cfg = &cfg;
    ESP_ERROR_CHECK(esp_netif_init());

    static bool loop_started = false;
    if (!loop_started) {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        loop_started = true;
    }
    if (g_sta_netif == nullptr) {
        g_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Conversation / ASR modes are the tightest internal-RAM cases (TLS +
    // WebSocket + mic DMA). Trim the static RX/TX buffers as before.
    if (cfg.operation_mode == config::OperationMode::Conversation ||
        cfg.operation_mode == config::OperationMode::AsrLocal) {
        wifi_cfg.static_rx_buf_num = 3;
        wifi_cfg.static_tx_buf_num = 4;
        if (wifi_cfg.rx_ba_win > 6) {
            wifi_cfg.rx_ba_win = 6;
        }
        ESP_LOGI(kTag, "Wi-Fi buffers trimmed for mode %u: static rx=%d tx=%d",
                 static_cast<unsigned>(cfg.operation_mode), wifi_cfg.static_rx_buf_num,
                 wifi_cfg.static_tx_buf_num);
    }
    if (!g_wifi_inited.load(std::memory_order_acquire)) {
        ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
        g_wifi_inited.store(true, std::memory_order_release);

        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &event_handler, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &event_handler, nullptr));

        const esp_timer_create_args_t args = {
            .callback = &sta_retry_timer_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sta-retry",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &g_sta_retry_timer));
    }

    copy_field(reinterpret_cast<std::uint8_t*>(g_configured_ssid),
               sizeof(g_configured_ssid), cfg.wifi_ssid.c_str());

    wifi_config_t sta_cfg{};
    copy_field(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg.wifi_ssid.c_str());
    copy_field(sta_cfg.sta.password, sizeof(sta_cfg.sta.password),
               cfg.wifi_password.c_str());

    // DHCP is the default; malformed saved static values intentionally fall
    // back to DHCP so a hand-edited NVS blob cannot strand the device.
    (void)apply_sta_ipv4(cfg.wifi_static_ip, cfg.wifi_gateway, cfg.wifi_dns);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    // Always start the driver. With an empty SSID this keeps the STA
    // interface ready for first-run scans/temporary connections; the event
    // handler suppresses the automatic connect.
    ESP_ERROR_CHECK(esp_wifi_start());

    if (cfg.wifi_ssid.empty()) {
        ESP_LOGI(kTag, "no SSID configured — Wi-Fi driver started, STA connect suppressed");
    } else if (g_setup_mode.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "setup mode active — stored SSID '%s' connect suppressed",
                 cfg.wifi_ssid.c_str());
    } else {
        ESP_LOGI(kTag, "connecting to SSID: %s", cfg.wifi_ssid.c_str());
    }
}

bool wifi_is_connected()
{
    return g_connected.load(std::memory_order_acquire);
}

std::vector<WifiScanResult> wifi_scan()
{
    std::vector<WifiScanResult> results;
    if (!g_wifi_inited.load(std::memory_order_acquire)) return results;

    stop_retry_timer();
    wifi_scan_config_t scan_cfg{};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "scan start failed: %s", esp_err_to_name(err));
        return results;
    }

    std::uint16_t count = 0;
    err = esp_wifi_scan_get_ap_num(&count);
    if (err != ESP_OK || count == 0) {
        ESP_LOGI(kTag, "scan complete: no networks");
        return results;
    }
    std::vector<wifi_ap_record_t> aps(count);
    err = esp_wifi_scan_get_ap_records(&count, aps.data());
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "scan get records failed: %s", esp_err_to_name(err));
        return results;
    }

    results.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto* ssid = reinterpret_cast<const char*>(aps[i].ssid);
        if (ssid[0] == '\0') continue;
        results.push_back(WifiScanResult{
            .ssid = ssid,
            .rssi = aps[i].rssi,
            .secure = aps[i].authmode != WIFI_AUTH_OPEN,
        });
    }
    // AP records contain one entry per BSSID. Sort strongest first, then drop
    // repeated SSIDs so the setup page offers each network only once.
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.rssi > b.rssi;
    });
    std::unordered_set<std::string> seen;
    results.erase(std::remove_if(results.begin(), results.end(),
                                 [&seen](const WifiScanResult& result) {
                                     return !seen.insert(result.ssid).second;
                                 }),
                  results.end());
    ESP_LOGI(kTag, "scan complete: %u unique networks", static_cast<unsigned>(results.size()));
    return results;
}

bool wifi_connect_temporary(const std::string& ssid, const std::string& password,
                            const std::string& static_ip, const std::string& gateway,
                            const std::string& dns)
{
    if (!g_wifi_inited.load(std::memory_order_acquire) || g_sta_netif == nullptr) {
        return false;
    }
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        ESP_LOGW(kTag, "temporary connect rejected: invalid SSID/password length");
        return false;
    }

    g_setup_mode.store(true, std::memory_order_release);
    stop_retry_timer();
    g_connected.store(false, std::memory_order_release);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(kTag, "disconnect before setup returned %s", esp_err_to_name(err));
    }

    wifi_config_t sta_cfg{};
    copy_field(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), ssid.c_str());
    copy_field(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), password.c_str());
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) return false;
    (void)apply_sta_ipv4(static_ip, gateway, dns);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "temporary connect start failed: %s", esp_err_to_name(err));
        return false;
    }

    for (int i = 0; i < 200; ++i) {
        if (g_connected.load(std::memory_order_acquire)) {
            ESP_LOGI(kTag, "temporary connection succeeded: SSID=%s IP=%s",
                     ssid.c_str(), wifi_ip_address().c_str());
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(kTag, "temporary connection timed out after 20 s: SSID=%s", ssid.c_str());
    return false;
}

std::string wifi_ip_address()
{
    if (g_sta_netif == nullptr) return {};
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(g_sta_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return {};
    }
    char ip[16] = {};
    std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return ip;
}

void wifi_set_setup_mode(bool active)
{
    const bool was_active = g_setup_mode.exchange(active, std::memory_order_acq_rel);
    if (active) {
        stop_retry_timer();
        ESP_LOGI(kTag, "setup mode enabled — automatic STA reconnect paused");
        return;
    }
    if (was_active) {
        ESP_LOGI(kTag, "setup mode disabled — automatic STA reconnect resumed");
        if (!g_connected.load(std::memory_order_acquire) &&
            !g_ap_active.load(std::memory_order_acquire) &&
            g_configured_ssid[0] != '\0' &&
            g_wifi_inited.load(std::memory_order_acquire)) {
            (void)esp_wifi_connect();
        }
    }
}

bool wifi_setup_mode()
{
    return g_setup_mode.load(std::memory_order_acquire);
}

// --- SoftAP provisioning ----------------------------------------------------

namespace {

void derive_ap_credentials()
{
    if (g_ap_ssid[0] != 0) return; // already derived
    std::uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    std::snprintf(g_ap_ssid, sizeof(g_ap_ssid),
                  "Stackchan-%02X%02X%02X", mac[3], mac[4], mac[5]);
    std::snprintf(g_ap_pw, sizeof(g_ap_pw),
                  "sc-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

void ap_post_start_worker(void*)
{
    if (g_boot_cfg != nullptr) {
        // wifi_config::start is idempotent — returns AlreadyStarted if the
        // server is already up from a previous STA_GOT_IP. The server binds
        // to INADDR_ANY so it serves both AP and STA interfaces.
        (void)wifi_config::start(*g_boot_cfg);
    }
    wifi_config::set_provisioning_mode(true);
    wifi_config::set_first_run_mode(g_ap_first_run.load(std::memory_order_acquire));
    captive_portal::start_dns();
    for (int i = 0; i < 40; ++i) {
        if (auto h = wifi_config::handle(); h != nullptr) {
            captive_portal::register_http_catchall(h);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(nullptr);
}

} // namespace

void wifi_enable_ap_mode(bool first_run)
{
    if (g_ap_active.load(std::memory_order_acquire)) {
        if (first_run) g_ap_first_run.store(true, std::memory_order_release);
        return;
    }

    g_ap_first_run.store(first_run, std::memory_order_release);
    if (first_run) wifi_set_setup_mode(true);
    derive_ap_credentials();
    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // APSTA keeps the STA side alive for candidate-network scans/connects.
    constexpr int kMaxRetries = 5;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < kMaxRetries; ++i) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) break;
        ESP_LOGW(kTag, "set_mode(APSTA) failed (try %d/%d): %s",
                 i + 1, kMaxRetries, esp_err_to_name(err));
        if (i + 1 < kMaxRetries) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "set_mode(APSTA) failed after %d tries: %s — AP not enabled",
                 kMaxRetries, esp_err_to_name(err));
        g_ap_first_run.store(false, std::memory_order_release);
        return;
    }

    wifi_config_t ap_cfg{};
    copy_field(ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), g_ap_ssid);
    ap_cfg.ap.ssid_len = std::strlen(g_ap_ssid);
    copy_field(ap_cfg.ap.password, sizeof(ap_cfg.ap.password), g_ap_pw);
    ap_cfg.ap.channel        = 6;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.pmf_cfg.required = false;
    if (esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg); cfg_err != ESP_OK) {
        ESP_LOGE(kTag, "set_config(AP) failed: %s — reverting to STA",
                 esp_err_to_name(cfg_err));
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        g_ap_first_run.store(false, std::memory_order_release);
        return;
    }

    const esp_err_t st = esp_wifi_start();
    if (st != ESP_OK && st != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "esp_wifi_start (AP) returned %s", esp_err_to_name(st));
    }

    if (g_ap_netif != nullptr) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(g_ap_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            std::snprintf(g_ap_ip, sizeof(g_ap_ip), IPSTR, IP2STR(&ip.ip));
        }
    }

    g_ap_active.store(true, std::memory_order_release);
    // Never log the AP password. It is shown on-device / in the QR payload.
    ESP_LOGI(kTag, "AP up: SSID=\"%s\" IP=%s first_run=%d",
             g_ap_ssid, g_ap_ip, static_cast<int>(first_run));

    if (xTaskCreate(&ap_post_start_worker, "ap-post-start", 3 * 1024, nullptr,
                    tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to spawn AP HTTP worker");
        g_ap_active.store(false, std::memory_order_release);
        g_ap_first_run.store(false, std::memory_order_release);
    }
}

bool wifi_disable_ap_mode()
{
    if (!g_ap_active.load(std::memory_order_acquire)) return true;

    const bool was_first_run = g_ap_first_run.load(std::memory_order_acquire);

    // Drop back to STA-only first, and only tear down the portal once that
    // succeeds. A failed set_mode leaves the AP fully alive so the UI can
    // retry rather than claiming success.
    constexpr int kMaxRetries = 5;
    esp_err_t err = ESP_OK;
    for (int i = 0; i < kMaxRetries; ++i) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) break;
        ESP_LOGW(kTag, "set_mode(STA) on AP-disable failed (try %d/%d): %s",
                 i + 1, kMaxRetries, esp_err_to_name(err));
        if (i + 1 < kMaxRetries) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "set_mode(STA) on AP-disable failed after %d tries: %s"
                       " — leaving AP up so it can be retried",
                 kMaxRetries, esp_err_to_name(err));
        return false;
    }

    captive_portal::stop_dns();
    if (auto h = wifi_config::handle(); h != nullptr) {
        captive_portal::unregister_http_catchall(h);
    }
    wifi_config::set_provisioning_mode(false);
    g_ap_active.store(false, std::memory_order_release);
    g_ap_client_count.store(0, std::memory_order_release);
    stop_retry_timer();

    // First-run finish deliberately keeps setup mode active and preserves the
    // accepted candidate association until the caller reboots into the saved
    // config. The normal settings-AP path resumes reconnecting immediately.
    if (!was_first_run) {
        wifi_set_setup_mode(false);
        if (!g_connected.load(std::memory_order_acquire)) {
            (void)esp_wifi_connect();
        }
    }
    g_ap_first_run.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "AP down");
    return true;
}

bool wifi_ap_active()
{
    return g_ap_active.load(std::memory_order_acquire);
}

bool wifi_ap_first_run()
{
    return g_ap_first_run.load(std::memory_order_acquire);
}

bool wifi_ap_info(char* ssid, std::size_t ssid_cap,
                  char* password, std::size_t pw_cap,
                  char* ip, std::size_t ip_cap)
{
    if (!g_ap_active.load(std::memory_order_acquire)) return false;
    if (ssid == nullptr || password == nullptr || ip == nullptr) return false;
    if (ssid_cap <= std::strlen(g_ap_ssid)) return false;
    if (pw_cap   <= std::strlen(g_ap_pw))   return false;
    if (ip_cap   <= std::strlen(g_ap_ip))   return false;
    std::strcpy(ssid, g_ap_ssid);
    std::strcpy(password, g_ap_pw);
    std::strcpy(ip, g_ap_ip);
    return true;
}

} // namespace stackchan::app
