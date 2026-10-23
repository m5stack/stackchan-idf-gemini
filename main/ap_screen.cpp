// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "ap_screen.hpp"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <config_service/product.hpp>
#include <lgfx/utility/lgfx_qrcode.h>
#include "first_run.hpp"
#include "wifi_sta.hpp"

namespace stackchan::app::ap_screen {

namespace {

// Track the last (SSID, PW, IP) tuple painted so we only redraw when the
// driver flips state. The QR encode + multi-rect fillRect is expensive on
// large round panels (StopWatch 466×466) and we don't want to burn it every
// frame at 30 fps.
char g_last_ssid[33] = {0};
char g_last_pw[33]   = {0};
char g_last_ip[16]   = {0};
bool g_last_first_run = false;
bool g_painted = false;

// Action-button rect in panel coords (set by the most recent draw call).
// handle_tap hit-tests against these. Boards without LCD touch (AtomS3R /
// AtomS3 — the "tiny" branch) leave btn_w = 0; handle_tap then never matches
// and still consumes the tap to keep the touchless gesture path clean.
int g_btn_x = 0;
int g_btn_y = 0;
int g_btn_w = 0;
int g_btn_h = 0;

// Build the standard Wi-Fi QR payload. Format documented by the Wi-Fi
// Alliance and supported natively by iOS Camera / Android scanners:
//   WIFI:T:<auth>;S:<SSID>;P:<password>;;
// Special chars `\`, `;`, `,`, `"`, `:` must be escaped with `\` inside SSID
// and password — our derived SSID/PW use only [A-Za-z0-9-], so no escaping
// is actually needed; we still apply it for safety should the credential
// derivation ever change. `H:false` is omitted because hidden is already the
// default; shortening the payload keeps it at QR version 4 / 33 modules.
std::size_t append_escaped(char* dst, std::size_t cap, std::size_t pos, const char* src)
{
    for (; *src != '\0' && pos + 2 < cap; ++src) {
        const char c = *src;
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') {
            dst[pos++] = '\\';
        }
        dst[pos++] = c;
    }
    dst[pos] = '\0';
    return pos;
}

void build_wifi_qr_payload(const char* ssid, const char* pw, char* out, std::size_t cap)
{
    std::size_t p = 0;
    constexpr const char* kPrefix = "WIFI:T:WPA;S:";
    constexpr const char* kMid    = ";P:";
    constexpr const char* kSuffix = ";;";
    p += std::snprintf(out + p, cap - p, "%s", kPrefix);
    p = append_escaped(out, cap, p, ssid);
    p += std::snprintf(out + p, cap - p, "%s", kMid);
    p = append_escaped(out, cap, p, pw);
    std::snprintf(out + p, cap - p, "%s", kSuffix);
}

// CoreS3's generated SSID is "StackChan-XXXXXX" in Gemini builds. Splitting at the hyphen
// makes the credential readable in the narrow column beside the QR without
// reducing the QR size or using an unreadably small font.
void draw_ssid_lines(M5GFX& display, const char* ssid, int x, int y, int line_h, int width)
{
    const char* split = std::strrchr(ssid, '-');
    if (split != nullptr && split != ssid && display.textWidth(ssid) > width) {
        char first[33];
        const std::size_t first_len = static_cast<std::size_t>(split - ssid) + 1;
        if (first_len < sizeof(first)) {
            std::memcpy(first, ssid, first_len);
            first[first_len] = '\0';
            display.drawString(first, x, y);
            display.drawString(split + 1, x, y + line_h);
            return;
        }
    }
    display.drawString(ssid, x, y);
}

// M5GFX's qrcode() helper hard-codes ECC low. Render the same QR utility
// directly at quartile ECC so the LCD code has more tolerance for glare,
// moire, and phone-camera noise. The Wi-Fi payload fits version 4 (33
// modules), preserving the 5 px module size in the CoreS3 layout.
bool draw_wifi_qrcode(M5GFX& display, const char* payload, int x, int y, int box)
{
    std::array<std::uint8_t, 512> modules{};
    constexpr std::uint8_t kMaxVersion = 10; // 512 B covers this payload range.

    for (std::uint8_t version = 1; version <= kMaxVersion; ++version) {
        QRCode qrcode{};
        if (lgfx_qrcode_initText(&qrcode, modules.data(), version,
                                 ECC_QUARTILE, payload) != 0) {
            continue;
        }

        const int thickness = box / qrcode.size;
        if (thickness <= 0) return false;
        const int code_size = qrcode.size * thickness;
        const int offset = (box - code_size) / 2;

        display.fillRect(x, y, box, box, TFT_WHITE);
        for (int iy = 0; iy < qrcode.size; ++iy) {
            for (int ix = 0; ix < qrcode.size; ++ix) {
                const auto colour = lgfx_qrcode_getModule(&qrcode, ix, iy)
                                        ? TFT_BLACK : TFT_WHITE;
                display.fillRect(x + offset + ix * thickness,
                                 y + offset + iy * thickness,
                                 thickness, thickness, colour);
            }
        }
        return true;
    }
    return false;
}

void draw_action_button(M5GFX& display, bool first_run)
{
    if (g_btn_w <= 0 || g_btn_h <= 0) return;

    const std::uint16_t red = display.color565(200, 90, 60);
    const std::uint16_t grey = display.color565(224, 230, 236);
    const std::uint16_t ink = display.color565(20, 35, 52);
    const std::uint16_t background = first_run ? grey : red;
    const std::uint16_t foreground = first_run ? ink : TFT_WHITE;

    display.fillRoundRect(g_btn_x, g_btn_y, g_btn_w, g_btn_h, 6, background);
    display.setFont(&fonts::lgfxJapanGothic_20);
    display.setTextColor(foreground, background);
    display.setTextDatum(lgfx::textdatum_t::middle_center);
    display.drawString(first_run ? config::tr("Skip", "スキップ")
                                : config::tr("Exit AP", "AP 停止"),
                       g_btn_x + g_btn_w / 2, g_btn_y + g_btn_h / 2);
}

} // namespace

bool active()
{
    return wifi_ap_active();
}

bool draw(M5GFX& display)
{
    char ssid[33], pw[33], ip[16];
    if (!wifi_ap_info(ssid, sizeof(ssid), pw, sizeof(pw), ip, sizeof(ip))) {
        return false;
    }
    const bool first_run = wifi_ap_first_run();
    // Only repaint when the credentials actually changed — saves the full
    // QR render cost on every render-task tick.
    if (g_painted &&
        std::strcmp(ssid, g_last_ssid) == 0 &&
        std::strcmp(pw, g_last_pw) == 0 &&
        std::strcmp(ip, g_last_ip) == 0 &&
        first_run == g_last_first_run) {
        return false;
    }
    // Use snprintf rather than strncpy(dst, src, sizeof-1): GCC 14 (IDF 5.5)
    // promotes -Wstringop-truncation to an error for the strncpy idiom because
    // it may leave the buffer unterminated. snprintf always NUL-terminates and
    // is accepted on both IDF 5.4 (GCC 13) and 5.5 (GCC 14).
    std::snprintf(g_last_ssid, sizeof(g_last_ssid), "%s", ssid);
    std::snprintf(g_last_pw, sizeof(g_last_pw), "%s", pw);
    std::snprintf(g_last_ip, sizeof(g_last_ip), "%s", ip);
    g_last_first_run = first_run;

    const std::int32_t W = display.width();
    const std::int32_t H = display.height();

    // Layout breakpoints:
    //   - tiny (≤160 px): 128×128 atom panels — no touch action; the Atom
    //                     status overlay owns the physical-button gestures.
    //   - mid:           CoreS3 320×240 — large QR on the left, enlarged
    //                    credentials and action button in the right column.
    //   - round (≥400):  StopWatch 466×466 AMOLED — centred QR with larger
    //                    text and a bottom action button.
    const bool tiny = W <= 160;
    const bool round_ = W >= 400;

    g_btn_x = 0;
    g_btn_y = 0;
    g_btn_w = 0;
    g_btn_h = 0;

    char qr_payload[160];
    build_wifi_qr_payload(ssid, pw, qr_payload, sizeof(qr_payload));

    display.startWrite();
    display.fillScreen(TFT_WHITE);

    if (tiny) {
        // 128×128: the largest code that still leaves room for SSID/PW.
        const int qr_w = W - 28;
        const int qr_x = (W - qr_w) / 2;
        const int qr_y = 8;
        // Version 1 lets M5GFX auto-increment to the smallest version that
        // fits. Version 0 never attempts an encode (the loop starts there).
        if (!draw_wifi_qrcode(display, qr_payload, qr_x, qr_y, qr_w)) {
            display.qrcode(qr_payload, qr_x, qr_y, qr_w, 1, false);
        }

        display.setFont(&fonts::Font0);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "SSID %s", ssid);
        display.drawString(buf, W / 2, qr_y + qr_w + 2);
        std::snprintf(buf, sizeof(buf), "PW %s", pw);
        display.drawString(buf, W / 2, qr_y + qr_w + 12);
    } else if (round_) {
        const int inset = static_cast<int>(W * (1.0f - 0.70710678f) * 0.5f) + 4;
        const int title_h = 32;
        const int top_margin = inset;
        const int btn_h = 44;
        const int btn_band_y = H - btn_h - 18;
        const int btn_w = 200;
        const int caption_lines = 3;
        const int caption_line_h = 22;
        const int caption_h = caption_lines * caption_line_h + 4;

        const int avail_y_for_qr =
            btn_band_y - top_margin - title_h - caption_h;
        const int qr_max_by_w = W * 220 / 466;
        const int qr_w = (avail_y_for_qr < qr_max_by_w) ? avail_y_for_qr : qr_max_by_w;
        const int qr_x = (W - qr_w) / 2;
        const int qr_y = top_margin + title_h;

        g_btn_x = (W - btn_w) / 2;
        g_btn_y = btn_band_y;
        g_btn_w = btn_w;
        g_btn_h = btn_h;

        display.setFont(&fonts::lgfxJapanGothic_24);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        display.drawString(config::tr("Wi-Fi setup", "Wi-Fi 設定モード"), W / 2, top_margin);

        // Explicit white space around the code plus stopWatch's white panel
        // gives the renderer a full quiet zone; no M5GFX `margin=true` path.
        if (!draw_wifi_qrcode(display, qr_payload, qr_x, qr_y, qr_w)) {
            display.qrcode(qr_payload, qr_x, qr_y, qr_w, 1, false);
        }

        display.setFont(&fonts::lgfxJapanGothic_20);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        int ty = qr_y + qr_w + 4;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSID: %s", ssid);
        display.drawString(buf, W / 2, ty);
        ty += caption_line_h;
        std::snprintf(buf, sizeof(buf), "PW:   %s", pw);
        display.drawString(buf, W / 2, ty);
        ty += caption_line_h;
        std::snprintf(buf, sizeof(buf), "http://%s/", ip);
        display.drawString(buf, W / 2, ty);

        draw_action_button(display, first_run);
    } else {
        // CoreS3. A 194 px box yields 5 px modules for the 33-module
        // version-4 payload: a 165 px printed code with 14–15 px of white
        // space inside the box. The surrounding white panel and 12 px gap to
        // the right column keep the required quiet zone intact.
        constexpr int kOuterMargin = 8;
        constexpr int kTitleY = 2;
        constexpr int kTitleH = 34;
        constexpr int kQrBox = 194;
        constexpr int kColumnGap = 12;
        constexpr int kButtonH = 44;
        constexpr int kButtonBottom = 12;

        const int qr_x = kOuterMargin;
        const int qr_y = kTitleY + kTitleH;
        const int column_x = qr_x + kQrBox + kColumnGap;
        const int column_w = W - column_x - kOuterMargin;
        const int button_y = H - kButtonH - kButtonBottom;

        g_btn_x = column_x;
        g_btn_y = button_y;
        g_btn_w = column_w;
        g_btn_h = kButtonH;

        display.setFont(&fonts::lgfxJapanGothic_24);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        display.drawString(config::tr("Wi-Fi setup", "Wi-Fi 設定モード"), W / 2, kTitleY);

        // Quartile ECC at version 4 keeps 5 px modules. The surrounding white
        // panel provides the quiet zone, and the title/text are spaced beyond
        // four module widths from the printed code.
        if (!draw_wifi_qrcode(display, qr_payload, qr_x, qr_y, kQrBox)) {
            display.qrcode(qr_payload, qr_x, qr_y, kQrBox, 1, false);
        }

        display.setFont(&fonts::lgfxJapanGothic_16);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_left);

        int ty = qr_y;
        display.drawString("SSID", column_x, ty);
        ty += 19;
        draw_ssid_lines(display, ssid, column_x, ty, 18, column_w);
        ty += 47;

        display.drawString("Password", column_x, ty);
        ty += 19;
        display.drawString(pw, column_x, ty);

        ty = button_y - 36;
        if (first_run) {
            // Keep the setup URL inside the narrow right column. The full
            // string is wider than the column even at this font size.
            display.setFont(&fonts::lgfxJapanGothic_12);
            display.drawString("http://", column_x, ty);
            char host[24];
            std::snprintf(host, sizeof(host), "%s/", ip);
            display.drawString(host, column_x, ty + 14);
        } else {
            display.setFont(&fonts::lgfxJapanGothic_16);
            display.drawString("Open setup", column_x, ty);
            display.drawString("in browser", column_x, ty + 17);
        }

        draw_action_button(display, first_run);
    }
    display.endWrite();

    g_painted = true;
    return true;
}

bool handle_tap(int x, int y)
{
    if (!wifi_ap_active()) return false;
    // While AP is up we own the touch input — swallow every tap even if it
    // missed the action button. Otherwise the tap would propagate to the
    // hidden device_ui underneath (whose top-right corner opens the 5-tab
    // settings UI), which would surprise the user when AP is later
    // dismissed and the UI reappears already open.
    if (g_btn_w > 0 && g_btn_h > 0 &&
        x >= g_btn_x && x < g_btn_x + g_btn_w &&
        y >= g_btn_y && y < g_btn_y + g_btn_h) {
        if (wifi_ap_first_run()) {
            if (first_run::skip_setup()) {
                g_painted = false;
            }
        } else if (wifi_disable_ap_mode()) {
            g_painted = false;
        }
    }
    return true;
}

} // namespace stackchan::app::ap_screen
