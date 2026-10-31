#include "cast_client.h"

#include <cJSON.h>
#include <esp_log.h>

#include <string>

#include "board.h"
#include "cjson_utils.h"

namespace {

constexpr char TAG[] = "CastClient";
// The service is on the LAN, so a slow answer means something is wrong rather
// than that a build is running.
constexpr int kRequestTimeoutMs = 6000;
// Starting projection makes the television fetch the first bytes of the
// stream, which takes a little longer than a listing.
constexpr int kCreateTimeoutMs = 12000;

std::string Trim(const std::string& value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return std::string();
    }
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string StringField(const cJSON* object, const char* key) {
    if (object == nullptr) {
        return std::string();
    }
    const cJSON* item = cJSON_GetObjectItem(object, key);
    return (item != nullptr && cJSON_IsString(item)) ? std::string(item->valuestring)
                                                     : std::string();
}

// ReadableError turns the service's error code into something worth saying out
// loud. Unknown codes fall back to the service's own message.
std::string ReadableError(const std::string& code, const std::string& message) {
    if (code == "target_busy") {
        return "这块电视正在播放别的内容，先停掉再投";
    }
    if (code == "target_offline") {
        return "这块电视现在离线了";
    }
    if (code == "unsupported_media") {
        return "这块电视不支持这种画面格式";
    }
    if (code == "device_control_failed") {
        return "电视没有响应投屏指令";
    }
    if (code == "invalid_request") {
        return "投屏请求不合法";
    }
    if (!message.empty()) {
        return message;
    }
    return "投屏服务出错了";
}

}  // namespace

void CastClient::Configure(const std::string& base_url) {
    base_url_ = Trim(base_url);
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

CastReply CastClient::GetTargets() {
    return Request("GET", "/v1/targets", std::string(), kRequestTimeoutMs);
}

CastReply CastClient::GetTarget(const std::string& target_id) {
    return Request("GET", "/v1/targets/" + target_id, std::string(), kRequestTimeoutMs);
}

CastReply CastClient::CreateSession(const std::string& request_json) {
    return Request("POST", "/v1/projection-sessions", request_json, kCreateTimeoutMs);
}

CastReply CastClient::StopSession(const std::string& session_id) {
    return Request("DELETE", "/v1/projection-sessions/" + session_id, std::string(),
                   kRequestTimeoutMs);
}

CastReply CastClient::Request(const std::string& method, const std::string& path,
                              const std::string& body, int timeout_ms) {
    CastReply reply;
    if (!Configured()) {
        reply.message = "还没有配置本地投屏服务地址";
        return reply;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        reply.message = "网络未连接";
        return reply;
    }
    // Slot 3 is what the other MCP HTTP tools use, so this connection stays
    // separate from OTA and asset downloads on slot 0.
    auto http = network->CreateHttp(3);
    if (http == nullptr) {
        reply.message = "无法创建 HTTP 客户端";
        return reply;
    }
    const std::string url = base_url_ + path;
    http->SetTimeout(timeout_ms);
    http->SetHeader("Accept", "application/json");
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(body));
    }

    if (auto opened = http->Open(method, url); !opened) {
        ESP_LOGW(TAG, "%s %s failed: %s", method.c_str(), url.c_str(),
                 opened.error().ToString().c_str());
        http->Close();
        reply.message = "连不上本地投屏服务，请检查现场网络";
        return reply;
    }
    if (auto code = http->GetStatusCode(); code) {
        reply.status = *code;
    }
    reply.body = http->ReadAll();
    http->Close();

    if (reply.status >= 200 && reply.status < 300) {
        reply.ok = true;
        return reply;
    }

    CJsonUniquePtr parsed(cJSON_Parse(reply.body.c_str()));
    const cJSON* error = cJSON_GetObjectItem(parsed.get(), "error");
    reply.code = StringField(error, "code");
    ESP_LOGW(TAG, "%s %s: HTTP %d %s", method.c_str(), path.c_str(), reply.status,
             reply.code.c_str());
    reply.message = ReadableError(reply.code, StringField(error, "message"));
    return reply;
}
