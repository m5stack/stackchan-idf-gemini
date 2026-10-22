// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "agent_delegate.hpp"

#include <config_service/product.hpp>
#include <cstring>

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>

namespace stackchan::app::agent_delegate {

namespace {

constexpr const char* kTag = "agent-delegate";
constexpr std::uint32_t kPollIntervalMs = 2000;
constexpr std::uint32_t kJobTimeoutMs = 180'000;
constexpr std::size_t kMaxQuestionBytes = 1000;
constexpr std::size_t kMaxResponseBytes = 8 * 1024;

std::string json_escape(const std::string& s)
{
    cJSON* node = cJSON_CreateString(s.c_str());
    char* q = cJSON_PrintUnformatted(node);
    cJSON_Delete(node);
    std::string out = q ? q : "\"\"";
    cJSON_free(q);
    return out;
}

std::string error_json(const std::string& msg)
{
    return R"({"ok":false,"error":)" + json_escape(msg) + "}";
}

} // namespace

Delegate::Delegate(std::string base_url, std::string token) : base_url_{std::move(base_url)}, token_{std::move(token)}
{
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

conversation::ToolDefinition Delegate::tool_definition()
{
    return conversation::ToolDefinition{
        .name = "ask_agent",
        .description = config::tr(
            "Ask a smarter home-server agent about things you cannot answer (search, math, "
            "looking things up). It takes tens of seconds; say you will check and wait. "
            "One request at a time.",
            "自分では分からないこと (最新情報の検索、計算、文章の下調べ、家のパソコンにある情報など) を、"
            "家のサーバーで動いている賢いエージェントに頼む。回答には数十秒かかるので、"
            "呼んだら「聞いてみるね」とだけ伝えて待つ。回答は後で自動的に届くので、届いたら要点を話す。"
            "同時に頼めるのは 1 件。"),
        .parameters_json = config::ui_japanese()
            ? R"({"type":"object","properties":{"question":{"type":"string","maxLength":1000,)"
              R"("description":"エージェントへの依頼内容。必要な前提も含めて日本語で具体的に"}},"required":["question"]})"
            : R"({"type":"object","properties":{"question":{"type":"string","maxLength":1000,)"
              R"("description":"The request for the agent, including needed context"}},"required":["question"]})",
        .non_blocking = true,
    };
}

std::optional<std::string> Delegate::http(const char* method, const std::string& path, const std::string& body,
                                          int& status_out)
{
    status_out = 0;
    const std::string url = base_url_ + path;
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    cfg.keep_alive_enable = false;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.method = std::strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return std::nullopt;
    if (!token_.empty()) {
        esp_http_client_set_header(client, "Authorization", ("Bearer " + token_).c_str());
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    std::optional<std::string> out;
    if (esp_http_client_open(client, static_cast<int>(body.size())) == ESP_OK) {
        bool ok = true;
        if (!body.empty()) {
            ok = esp_http_client_write(client, body.data(), static_cast<int>(body.size())) ==
                 static_cast<int>(body.size());
        }
        if (ok && esp_http_client_fetch_headers(client) >= 0) {
            status_out = esp_http_client_get_status_code(client);
            std::string resp;
            char buf[512];
            for (;;) {
                const int n = esp_http_client_read(client, buf, sizeof(buf));
                if (n <= 0) break;
                resp.append(buf, static_cast<std::size_t>(n));
                if (resp.size() > kMaxResponseBytes) break;
            }
            out = std::move(resp);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!out) ESP_LOGW(kTag, "%s %s failed (no response)", method, url.c_str());
    return out;
}

std::string Delegate::start(const std::string& arguments_json)
{
    if (!configured()) return error_json("エージェント サーバーの URL が設定されていません");
    if (busy()) return error_json("前の依頼の回答を待っています。届いてから次を頼んでください");

    std::string question;
    if (cJSON* root = cJSON_Parse(arguments_json.c_str()); root != nullptr) {
        const cJSON* q = cJSON_GetObjectItemCaseSensitive(root, "question");
        if (cJSON_IsString(q) && q->valuestring) question = q->valuestring;
        cJSON_Delete(root);
    }
    if (question.empty()) return error_json("question が必要です");
    if (question.size() > kMaxQuestionBytes) question.resize(kMaxQuestionBytes);

    int status = 0;
    const auto resp = http("POST", "/ask", R"({"question":)" + json_escape(question) + "}", status);
    if (!resp) return error_json("エージェント サーバーに接続できません");
    if (status != 200 && status != 202) {
        ESP_LOGW(kTag, "POST /ask -> %d: %.*s", status, 200, resp->c_str());
        return error_json(status == 401 ? "エージェント サーバーのトークンが違います"
                                       : "エージェント サーバーがエラーを返しました");
    }
    std::string id;
    if (cJSON* root = cJSON_Parse(resp->c_str()); root != nullptr) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (cJSON_IsString(v) && v->valuestring) id = v->valuestring;
        cJSON_Delete(root);
    }
    if (id.empty()) return error_json("エージェント サーバーの応答が不正です");

    job_id_ = std::move(id);
    question_ = std::move(question);
    started_ms_ = 0; // set on first poll (caller's clock)
    next_poll_ms_ = 0;
    ESP_LOGI(kTag, "job %s started: %s", job_id_.c_str(), question_.c_str());
    return R"({"ok":true,"status":"accepted","note":"回答は数十秒後に自動で届きます。ユーザーには待ってもらうよう短く伝えてください"})";
}

std::optional<Result> Delegate::poll(std::uint32_t now_ms)
{
    if (!busy()) return std::nullopt;
    if (started_ms_ == 0) started_ms_ = now_ms;
    if (static_cast<std::int32_t>(now_ms - next_poll_ms_) < 0) return std::nullopt;
    next_poll_ms_ = now_ms + kPollIntervalMs;

    auto finish = [this](Result r) {
        job_id_.clear();
        return std::optional<Result>{std::move(r)};
    };

    if (now_ms - started_ms_ > kJobTimeoutMs) {
        ESP_LOGW(kTag, "job %s timed out", job_id_.c_str());
        return finish(Result{.ok = false,
                             .error = config::tr("The agent did not answer in time",
                                                 "エージェントからの回答が時間内に届きませんでした")});
    }

    int status = 0;
    const auto resp = http("GET", "/jobs/" + job_id_, "", status);
    if (!resp) return std::nullopt; // transient; retry next interval
    if (status == 404) return finish(Result{.ok = false,
                                            .error = config::tr("The agent server lost the request",
                                                                "エージェント サーバーが依頼を見失いました")});
    if (status != 200) return std::nullopt;

    cJSON* root = cJSON_Parse(resp->c_str());
    if (root == nullptr) return std::nullopt;
    const cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "status");
    const std::string state = cJSON_IsString(st) && st->valuestring ? st->valuestring : "";
    std::optional<Result> out;
    if (state == "done") {
        Result r;
        r.ok = true;
        const cJSON* a = cJSON_GetObjectItemCaseSensitive(root, "answer");
        const cJSON* rd = cJSON_GetObjectItemCaseSensitive(root, "reading");
        if (cJSON_IsString(a) && a->valuestring) r.answer = a->valuestring;
        if (cJSON_IsString(rd) && rd->valuestring) r.reading = rd->valuestring;
        ESP_LOGI(kTag, "job %s done (%u B)", job_id_.c_str(), static_cast<unsigned>(r.answer.size()));
        out = finish(std::move(r));
    } else if (state == "error") {
        const cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
        Result r;
        r.error = cJSON_IsString(e) && e->valuestring ? e->valuestring : "エージェントがエラーになりました";
        ESP_LOGW(kTag, "job %s error: %s", job_id_.c_str(), r.error.c_str());
        out = finish(std::move(r));
    }
    cJSON_Delete(root);
    return out;
}

} // namespace stackchan::app::agent_delegate
