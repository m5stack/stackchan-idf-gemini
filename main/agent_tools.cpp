// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "agent_tools.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <conversation/json.hpp>

#include "settings_sinks.hpp"
#include "utf8.hpp"
#include <config_service/product.hpp>

namespace stackchan::app::agent_tools {

namespace conv = stackchan::conversation;
using namespace ArduinoJson;
using conv::make_json_doc;
using conv::json_to_string;

namespace {

constexpr const char* kTag = "agent-tools";

// ---- memory store: NVS namespace "memory" on the 1 MB "storage" partition ---
// (the main "nvs" partition is 16 KiB and essentially full). One JSON blob
// {"items":[{"k":...,"v":...,"t":<unix>}]}; small enough to rewrite whole.
constexpr const char* kPart = "storage";
constexpr const char* kNs = "memory";
constexpr const char* kKey = "items";
constexpr std::size_t kMaxItems = 32;
constexpr std::size_t kMaxKeyBytes = 48;
constexpr std::size_t kMaxValueBytes = 500;

bool ensure_partition()
{
    static int state = 0; // 0 unknown, 1 ok, -1 absent
    if (state != 0) return state == 1;
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    if (part == nullptr) {
        state = -1;
        return false;
    }
    // Another user (avatar_vm / voice_db) may already have initialised it.
    const esp_err_t err = nvs_flash_init_partition_ptr(part);
    state = (err == ESP_OK || err == ESP_ERR_INVALID_STATE) ? 1 : -1;
    if (state != 1) ESP_LOGW(kTag, "nvs init on \"%s\" failed: %s", kPart, esp_err_to_name(err));
    return state == 1;
}

// Loaded items as the "items" array of `doc` (empty array when nothing stored).
JsonArray load_items(JsonDocument& doc)
{
    nvs_handle_t h;
    if (ensure_partition() && nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) == ESP_OK) {
        std::size_t len = 0;
        if (nvs_get_blob(h, kKey, nullptr, &len) == ESP_OK && len > 0 && len < 64 * 1024) {
            std::string buf(len, '\0');
            if (nvs_get_blob(h, kKey, buf.data(), &len) == ESP_OK) {
                (void)deserializeJson(doc, buf);
            }
        }
        nvs_close(h);
    }
    if (!doc["items"].is<JsonArray>()) {
        doc.clear();
        return doc["items"].to<JsonArray>();
    }
    return doc["items"].as<JsonArray>();
}

bool save_items(const JsonDocument& doc)
{
    std::string text;
    serializeJson(doc, text);
    bool ok = false;
    nvs_handle_t h;
    if (ensure_partition() && nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_blob(h, kKey, text.c_str(), text.size() + 1) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    return ok;
}

std::string error_json(const char* msg)
{
    return std::string(R"({"ok":false,"error":)") + conv::json_quote(msg) + "}";
}

// ---- get_time ---------------------------------------------------------------

std::string tool_get_time()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2020) {
        return config::ui_japanese()
                   ? R"json({"ok":false,"error":"時計がまだ合っていません (Wi-Fi 未接続か NTP 未同期)"})json"
                   : R"json({"ok":false,"error":"clock not set yet (Wi-Fi down or NTP not synced)"})json";
    }
    static const char* kWeekdayJa[] = {"日", "月", "火", "水", "木", "金", "土"};
    static const char* kWeekdayEn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* const* kWeekday = config::ui_japanese() ? kWeekdayJa : kWeekdayEn;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  R"({"ok":true,"date":"%04d-%02d-%02d","time":"%02d:%02d","weekday":"%s","timezone":"JST"})",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, kWeekday[tm.tm_wday]);
    return buf;
}

// ---- get_status -------------------------------------------------------------

std::string tool_get_status(SharedState& state)
{
    std::string out = R"({"ok":true)";
    const auto& px = state.proximity;
    if (px.available.load(std::memory_order_relaxed)) {
        char b[96];
        std::snprintf(b, sizeof(b), R"(,"proximity":{"hand_near":%s,"raw":%u})",
                      px.near.load(std::memory_order_relaxed) ? "true" : "false",
                      static_cast<unsigned>(px.raw.load(std::memory_order_relaxed)));
        out += b;
    }
    const int pct = state.battery.pct.load(std::memory_order_relaxed);
    const int mv = state.battery.mv.load(std::memory_order_relaxed);
    if (pct >= 0 || mv >= 0) {
        char b[96];
        std::snprintf(b, sizeof(b), R"(,"battery":{"percent":%d,"millivolt":%d,"milliamp":%d})", pct, mv,
                      static_cast<int>(state.battery.ma.load(std::memory_order_relaxed)));
        out += b;
    }
    {
        char b[64];
        std::snprintf(b, sizeof(b), R"(,"uptime_min":%u)",
                      static_cast<unsigned>(esp_timer_get_time() / 60'000'000));
        out += b;
    }
    if (state.lt.active.load(std::memory_order_relaxed)) {
        char b[64];
        std::snprintf(b, sizeof(b), R"(,"lt_timer_remaining_s":%d)",
                      static_cast<int>(state.lt.remaining_s.load(std::memory_order_relaxed)));
        out += b;
    }
    out += "}";
    return out;
}

// ---- remember / recall / forget ----------------------------------------------

std::string tool_remember(const std::string& args)
{
    auto in = make_json_doc();
    if (deserializeJson(in, args)) return error_json("bad arguments");
    std::string key = in["key"] | "";
    std::string text = in["text"] | "";
    if (key.empty() || text.empty()) return error_json(config::tr("key and text are required", "key と text が必要です"));
    if (key.size() > kMaxKeyBytes) key.resize(kMaxKeyBytes);
    if (text.size() > kMaxValueBytes) return error_json(config::tr("text is too long (500 bytes max)", "text が長すぎます (500 バイトまで)"));

    auto store = make_json_doc();
    JsonArray arr = load_items(store);
    // Replace an existing key in place; otherwise append (dropping the
    // oldest when full).
    JsonObject item;
    for (JsonObject it : arr) {
        if (key == (it["k"] | "")) { item = it; break; }
    }
    if (item.isNull()) {
        while (arr.size() >= kMaxItems) arr.remove(0);
        item = arr.add<JsonObject>();
        item["k"] = key;
    }
    item["v"] = text;
    item["t"] = static_cast<double>(std::time(nullptr));
    const std::size_t n = arr.size();
    if (!save_items(store)) return error_json(config::tr("save failed", "保存に失敗しました"));
    ESP_LOGI(kTag, "remember: %s (%u items)", key.c_str(), static_cast<unsigned>(n));
    char b[64];
    std::snprintf(b, sizeof(b), R"({"ok":true,"stored":%u})", static_cast<unsigned>(n));
    return b;
}

std::string tool_recall(const std::string& args)
{
    auto in = make_json_doc();
    std::string key;
    if (!deserializeJson(in, args)) key = in["key"] | "";
    auto store = make_json_doc();
    JsonArray arr = load_items(store);
    auto out = make_json_doc();
    out["ok"] = true;
    JsonArray list = out["memories"].to<JsonArray>();
    for (JsonObjectConst it : arr) {
        const char* ik = it["k"].as<const char*>();
        const char* iv = it["v"].as<const char*>();
        if (ik == nullptr || iv == nullptr) continue;
        if (!key.empty() && key != ik) continue;
        JsonObject e = list.add<JsonObject>();
        e["key"] = ik;
        e["text"] = iv;
    }
    return json_to_string(out);
}

std::string tool_forget(const std::string& args)
{
    auto in = make_json_doc();
    if (deserializeJson(in, args)) return error_json("bad arguments");
    const std::string key = in["key"] | "";
    if (key.empty()) return error_json(config::tr("key is required", "key が必要です"));
    auto store = make_json_doc();
    JsonArray arr = load_items(store);
    int removed = 0;
    for (std::size_t i = arr.size(); i-- > 0;) {
        if (key == (arr[i]["k"] | "")) { arr.remove(i); ++removed; }
    }
    if (!save_items(store)) return error_json(config::tr("save failed", "保存に失敗しました"));
    char b[64];
    std::snprintf(b, sizeof(b), R"({"ok":true,"removed":%d})", removed);
    return b;
}

// ---- set_timer --------------------------------------------------------------

std::string tool_set_timer(const std::string& args, SharedState& state)
{
    auto in = make_json_doc();
    if (deserializeJson(in, args)) return error_json("bad arguments");
    const double seconds = in["seconds"] | -1.0;
    std::string display = in["message"] | "";
    std::string reading = in["reading"] | "";
    if (seconds <= 0 || seconds > 12 * 3600)
        return error_json(config::tr("seconds must be between 1 and 43200",
                                     "seconds は 1〜43200 の範囲で指定してください"));
    if (display.empty()) display = config::tr("It's time", "じかんだよ");
    if (reading.empty()) {
        // Japanese formant TTS cannot speak English; leave empty so demo_loop
        // falls back to the balloon only.
        reading = config::ui_japanese() ? display : std::string{};
    }
    if (display.size() > 120) display.resize(120);
    if (reading.size() > 120) reading.resize(120);
    const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    const std::uint32_t due_ms = now_ms + static_cast<std::uint32_t>(seconds * 1000.0);
    state.set_reminder(due_ms, display, reading);
    ESP_LOGI(kTag, "set_timer: %u s -> \"%s\"", static_cast<unsigned>(seconds), display.c_str());
    char b[64];
    std::snprintf(b, sizeof(b), R"({"ok":true,"fires_in_s":%u})", static_cast<unsigned>(seconds));
    return b;
}

} // namespace

// set_volume: {"action":"up"|"down"|"mute"|"unmute"|"set", "level": 0..200}
// Percent of the board's base volume (100 = factory default); up/down step
// by 20. Mute is session-only (SharedState flag, demo_loop re-applies the
// master volume on the edge, same as the corner-tap mute); the percent is
// persisted so it survives a reboot like the settings-page slider.
std::string tool_set_volume(const std::string& args, SharedState& state)
{
    auto in = make_json_doc();
    (void)deserializeJson(in, args);
    const std::string action = in["action"] | "";
    const int level = in["level"] | -1;
    constexpr int kStep = 20, kMin = 10, kMax = 200;
    int pct = static_cast<int>(state.speaker.volume_pct.load(std::memory_order_relaxed));
    bool muted = state.speaker.muted.load(std::memory_order_relaxed);
    if (action == "mute") {
        muted = true;
    } else if (action == "unmute") {
        muted = false;
    } else if (action == "up") {
        pct = std::min(kMax, pct + kStep);
        muted = false;
    } else if (action == "down") {
        pct = std::max(kMin, pct - kStep);
    } else if (action == "set") {
        if (level < 0 || level > kMax)
            return config::ui_japanese() ? R"({"ok":false,"error":"level は 0〜200 で指定"})"
                                         : R"({"ok":false,"error":"level must be 0..200"})";
        pct = std::max(kMin, level);
        muted = false;
    } else {
        return config::ui_japanese()
                   ? R"({"ok":false,"error":"action は up / down / mute / unmute / set のいずれか"})"
                   : R"({"ok":false,"error":"action must be up / down / mute / unmute / set"})";
    }
    state.speaker.muted.store(muted, std::memory_order_relaxed);
    settings_sinks::apply_speaker_volume_persist(static_cast<std::uint16_t>(pct));
    char buf[96];
    std::snprintf(buf, sizeof(buf), R"({"ok":true,"volume_pct":%d,"muted":%s})", pct, muted ? "true" : "false");
    ESP_LOGI(kTag, "set_volume %s -> %d%% muted=%d", action.c_str(), pct, muted ? 1 : 0);
    return buf;
}

// lt_timer: {"action":"start"|"stop"|"status", "minutes": 0.5..180}
// Drives the same LT (lightning-talk) timekeeper as the on-device LT tab:
// countdown on screen, "のこり1分" warning, exact-time and overtime
// announcements (spoken by the assistant while a conversation is live).
std::string tool_lt_timer(const std::string& args, SharedState& state)
{
    auto in = make_json_doc();
    (void)deserializeJson(in, args);
    const std::string action = in["action"] | "";
    const double minutes = in["minutes"] | 0.0;
    char buf[160];
    if (action == "start") {
        if (minutes < 0.5 || minutes > 180)
            return config::ui_japanese() ? R"({"ok":false,"error":"minutes は 0.5〜180 で指定"})"
                                         : R"({"ok":false,"error":"minutes must be 0.5..180"})";
        const auto total_s = static_cast<std::uint16_t>(minutes * 60.0 + 0.5);
        state.lt.total_s.store(total_s, std::memory_order_relaxed);
        state.lt.command.store(1, std::memory_order_release);
        ESP_LOGI(kTag, "lt_timer start %u s", static_cast<unsigned>(total_s));
        std::snprintf(buf, sizeof(buf), R"({"ok":true,"started":true,"total_s":%u})", static_cast<unsigned>(total_s));
        return buf;
    }
    if (action == "stop") {
        state.lt.command.store(2, std::memory_order_release);
        ESP_LOGI(kTag, "lt_timer stop");
        return R"({"ok":true,"stopped":true})";
    }
    if (action == "status") {
        const bool active = state.lt.active.load(std::memory_order_relaxed);
        std::snprintf(buf, sizeof(buf), R"({"ok":true,"active":%s,"remaining_s":%d,"total_s":%u})",
                      active ? "true" : "false", static_cast<int>(state.lt.remaining_s.load(std::memory_order_relaxed)),
                      static_cast<unsigned>(state.lt.total_s.load(std::memory_order_relaxed)));
        return buf;
    }
    return config::ui_japanese() ? R"({"ok":false,"error":"action は start / stop / status のいずれか"})"
                                 : R"({"ok":false,"error":"action must be start / stop / status"})";
}

std::vector<conv::ToolDefinition> definitions()
{
    const bool ja = config::ui_japanese();
    return {
        conv::ToolDefinition{
            .name = "lt_timer",
            .description = ja
                ? "発表 (LT) 用のカウントダウン タイマー。action: start (minutes で長さを指定、例 1 や 5)、"
                  "stop (停止・リセット)、status (残り時間)。画面に残り時間を表示し、残り 1 分・終了・超過を声で知らせる。"
                  "「3 分のタイマーをセット」「タイマー止めて」「あと何分？」などで使う。"
                  "単純に N 分後に一言知らせてほしいだけなら set_timer を使う。"
                : "Lightning-talk countdown timer. action: start (minutes, e.g. 1 or 5), "
                  "stop, status. Shows remaining time on screen. In English mode announcements "
                  "are on-screen only (no Japanese TTS). Use set_timer for a one-shot reminder.",
            .parameters_json =
                R"({"type":"object","properties":{"action":{"type":"string","enum":["start","stop","status"]},)"
                R"("minutes":{"type":"number","minimum":0.5,"maximum":180}},"required":["action"]})",
        },
        conv::ToolDefinition{
            .name = "set_volume",
            .description = ja
                ? "スピーカーの音量を変える / ミュートする。action: up (少し上げる), down (少し下げる), "
                  "mute (消音), unmute (消音解除), set (level で 0〜200% を直接指定、100 が標準)。"
                  "「音量上げて」「うるさい」「静かにして」「ミュート」「音出して」などで使う。"
                : "Change speaker volume or mute. action: up, down, mute, unmute, set "
                  "(level 0..200 percent, 100 = default).",
            .parameters_json =
                R"({"type":"object","properties":{"action":{"type":"string","enum":["up","down","mute","unmute","set"]},)"
                R"("level":{"type":"integer","minimum":0,"maximum":200}},"required":["action"]})",
        },
        conv::ToolDefinition{
            .name = "get_time",
            .description = ja ? "現在の日付・時刻・曜日 (日本時間) を取得する。時間や日付を聞かれたら必ずこれで確認する。"
                              : "Get the current date, time, and weekday (JST). Always call this instead of guessing.",
            .parameters_json = R"({"type":"object","properties":{}})",
        },
        conv::ToolDefinition{
            .name = "get_status",
            .description = ja
                ? "スタックチャン自身の状態を取得する: 近接センサー (手が近くにあるか)、バッテリー残量、"
                  "起動からの時間、LT タイマーの残り。体調や電池、近くに何かあるか聞かれたときに使う。"
                : "Read Stack-chan's own status: proximity (hand nearby), battery, uptime, LT timer remaining.",
            .parameters_json = R"({"type":"object","properties":{}})",
        },
        conv::ToolDefinition{
            .name = "remember",
            .description = ja
                ? "ユーザーに頼まれたことや大事な情報を、電源を切っても残るメモとして保存する。"
                  "key は短い見出し (例: \"ユーザーの名前\")、text は内容。同じ key は上書き。"
                : "Persist a short note across reboots. key is a heading, text is the content. Same key overwrites.",
            .parameters_json = R"({"type":"object","properties":{"key":{"type":"string","maxLength":48},)"
                               R"("text":{"type":"string","maxLength":500}},"required":["key","text"]})",
        },
        conv::ToolDefinition{
            .name = "recall",
            .description = ja ? "保存してあるメモを読み出す。key を省略すると全部返す。以前の話や約束を聞かれたら使う。"
                              : "Recall stored notes. Omit key to list all.",
            .parameters_json = R"({"type":"object","properties":{"key":{"type":"string"}}})",
        },
        conv::ToolDefinition{
            .name = "forget",
            .description = ja ? "保存してあるメモを key を指定して消す。" : "Delete a stored note by key.",
            .parameters_json = R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})",
        },
        conv::ToolDefinition{
            .name = "set_timer",
            .description = ja
                ? "指定秒数後にスタックチャンが自分から声をかける (タイマー / リマインダー)。"
                  "message は画面に出す文、reading はその読み (ひらがな・カタカナのみ、漢字不可)。"
                  "設定できるのは 1 件だけで、再起動すると消える。"
                : "One-shot reminder after N seconds. message is shown on screen. "
                  "reading (hiragana) is optional and ignored in English mode (text balloon only). "
                  "One reminder at a time; lost on reboot.",
            .parameters_json = ja
                ? R"({"type":"object","properties":{"seconds":{"type":"integer","minimum":1,"maximum":43200},)"
                  R"("message":{"type":"string","maxLength":60},"reading":{"type":"string","maxLength":80}},)"
                  R"("required":["seconds","message","reading"]})"
                : R"({"type":"object","properties":{"seconds":{"type":"integer","minimum":1,"maximum":43200},)"
                  R"("message":{"type":"string","maxLength":60},"reading":{"type":"string","maxLength":80}},)"
                  R"("required":["seconds","message"]})",
        },
    };
}

const char* guidance()
{
    return config::ui_japanese()
               ? "\n\n[ツール] 時刻や日付を聞かれたら get_time、電池や近くの様子を聞かれたら get_status を使って"
                 "確かめてから答えてください (推測で答えない)。覚えておいてと言われたら remember、"
                 "前に話したことを聞かれたら recall を使います。「N 分後に教えて」は set_timer を使います "
                 "(reading はひらがなで)。音量やミュートの指示は set_volume、「N 分のタイマーをセット」は lt_timer を使います。ツールは必ず関数呼び出し (function calling) の仕組みで呼び、"
                 "「tool_code」や関数名を文章・音声として話さないでください。ツールの結果は要点だけ短く話してください。"
               : "\n\n[Tools] Use get_time for the date or time, and get_status for battery / "
                 "proximity / uptime — do not guess. Use remember / recall / forget for notes. "
                 "Use set_timer for a one-shot reminder (message only; do not rely on reading). "
                 "Use set_volume for loudness, lt_timer for a countdown on screen. Always invoke "
                 "tools via function calling; never speak tool names or tool_code. Summarise tool "
                 "results briefly.";
}

std::optional<std::string> dispatch(const std::string& name, const std::string& args, SharedState& state)
{
    if (name == "get_time") return tool_get_time();
    if (name == "get_status") return tool_get_status(state);
    if (name == "remember") return tool_remember(args);
    if (name == "recall") return tool_recall(args);
    if (name == "forget") return tool_forget(args);
    if (name == "set_timer") return tool_set_timer(args, state);
    if (name == "set_volume") return tool_set_volume(args, state);
    if (name == "lt_timer") return tool_lt_timer(args, state);
    return std::nullopt;
}

} // namespace stackchan::app::agent_tools
