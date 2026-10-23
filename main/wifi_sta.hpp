// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <config_service/config_service.hpp>

namespace stackchan::app {

// Start the Wi-Fi station using credentials from cfg. If cfg.wifi_ssid is empty
// the driver is initialised but no connection is attempted; wifi_is_connected()
// will stay false and demo_loop will show the "Wi-Fi: 切断中" balloon.
//
// Returns as soon as the driver is started — connection happens asynchronously.
void wifi_start(const config::DeviceConfig& cfg);

// True once Wi-Fi has an IP address. Becomes false again on disconnect.
bool wifi_is_connected();

// Current STA IPv4 address as a dotted-decimal string, or empty when no lease
// has been assigned yet. Used by first-run status and connection reporting.
std::string wifi_ip_address();

// --- First-run setup helpers ------------------------------------------------
// These are called from the first-run worker, never directly from the HTTP
// task. While setup mode is active automatic STA reconnect is suppressed so a
// failed password test cannot disrupt the SoftAP page. The station remains in
// the successful candidate network after connect_temporary(), allowing the
// caller to tear down the AP without dropping the association.
struct WifiScanResult {
    std::string ssid;
    std::int8_t rssi = 0;
    bool secure = true;
};

std::vector<WifiScanResult> wifi_scan();

// Apply DHCP/static IPv4 settings, disconnect any previous attempt, configure
// and join the candidate network. Blocks for up to 20 seconds waiting for an
// IPv4 address. `static_ip` empty selects DHCP; otherwise a /24 prefix and a
// gateway are required. `dns` may be empty, in which case the gateway is used.
bool wifi_connect_temporary(const std::string& ssid, const std::string& password,
                            const std::string& static_ip, const std::string& gateway,
                            const std::string& dns);

// Enter/leave first-run setup mode. Entering cancels pending normal reconnect
// timers; leaving re-enables normal reconnect behaviour.
void wifi_set_setup_mode(bool active);
bool wifi_setup_mode();

// --- SoftAP provisioning ---
// Switch Wi-Fi into APSTA so iOS (no Web Bluetooth) can join the device's own
// AP and use the existing wifi_config HTTP settings page to configure home
// Wi-Fi. STA stays attempting reconnect, so a successful STA association does
// not tear the AP down — the user can keep editing settings until they tap
// "AP mode" again on the device UI.
//
// Enable also (idempotently) starts wifi_config_service, flips it into
// provisioning mode (require_auth bypass + provisioning_mode flag in
// /api/status), and brings up the captive portal (DNS hijack + 404 catch-all
// pointing to the settings page).
//   - SSID = "Stackchan-XXXXXX" (last 3 STA MAC bytes — same as BLE / mDNS;
//     Gemini builds use the customer spelling "StackChan-XXXXXX").
//   - WPA2 PSK = "sc-XXXXXXXX"   (last 4 STA MAC bytes, lower hex).
// Both idempotent.
void wifi_enable_ap_mode(bool first_run = false);
// Returns false when the driver could not leave APSTA mode. The AP remains up
// in that case so the caller can retry instead of reporting a false success.
bool wifi_disable_ap_mode();
bool wifi_ap_active();
bool wifi_ap_first_run();

// AP join info for the on-device QR screen. Returns false if the AP isn't up
// or the buffers are too small.
bool wifi_ap_info(char* ssid, std::size_t ssid_cap,
                  char* password, std::size_t pw_cap,
                  char* ip, std::size_t ip_cap);

} // namespace stackchan::app
