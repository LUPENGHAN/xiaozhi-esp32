#include "cast_tools.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <string>

#include "cjson_utils.h"
#include "settings.h"

namespace {

constexpr char TAG[] = "Cast";

// The stream the control plane serves and the television pulls directly.
constexpr char kFormat[] = "mpegts";
constexpr char kVideoCodec[] = "h264";
constexpr char kAudioCodec[] = "aac";

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

std::string PrintJson(const cJSON* value) {
    CJsonStringUniquePtr json(cJSON_PrintUnformatted(value));
    return json != nullptr ? std::string(json.get()) : std::string();
}

// HasValue reports whether a JSON array of strings contains value.
bool HasValue(const cJSON* array, const char* value) {
    if (!cJSON_IsArray(array)) {
        return false;
    }
    for (const cJSON* item = array->child; item != nullptr; item = item->next) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, value) == 0) {
            return true;
        }
    }
    return false;
}

// SupportsStream reports whether an endpoint can play the cloud stream as is:
// progressive mpegts with h264 video and aac audio.
bool SupportsStream(const cJSON* endpoint) {
    const cJSON* capabilities = cJSON_GetObjectItem(endpoint, "capabilities");
    if (!cJSON_IsObject(capabilities)) {
        return false;
    }
    return cJSON_IsTrue(cJSON_GetObjectItem(capabilities, "streaming")) &&
           HasValue(cJSON_GetObjectItem(capabilities, "formats"), kFormat) &&
           HasValue(cJSON_GetObjectItem(capabilities, "video_codecs"), kVideoCodec) &&
           HasValue(cJSON_GetObjectItem(capabilities, "audio_codecs"), kAudioCodec);
}

}  // namespace

CastTools& CastTools::GetInstance() {
    static CastTools instance;
    return instance;
}

void CastTools::Initialize() {
    if (initialized_) {
        return;
    }
    // Compile-time default, overridable at runtime through the "casting" NVS
    // namespace so a device can be repointed without a new build.
    Settings settings("casting", false);
    std::string url = Trim(settings.GetString("url", CONFIG_CASTING_URL));
    if (url.empty()) {
        ESP_LOGI(TAG, "casting disabled: no local casting URL configured");
        return;
    }
    initialized_ = true;
    client_.Configure(url);

    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.cast.list_screens",
                "查询现场可以投屏的电视及其状态。用户要投屏、或想知道投到哪块屏幕时先调用本工具，"
                "拿到 target_id。",
                PropertyList(), [this](const PropertyList& p) { return ListScreens(p); });

    mcp.AddTool("self.cast.cast",
                "把云端已经启动的应用画面投到现场的一块电视上。url 使用控制面 start-app 返回的 "
                "stream_url，target_id 必须来自 self.cast.list_screens。投屏前先向用户确认要投到"
                "哪块屏幕。",
                PropertyList({Property("url", kPropertyTypeString).SetMaxLength(2048),
                              Property("target_id", kPropertyTypeString).SetMaxLength(128)}),
                [this](const PropertyList& p) { return Cast(p); });

    mcp.AddTool("self.cast.stop",
                "停止正在投屏的内容，电视回到空闲。不传 session_id 时停止本设备最近一次投屏。",
                PropertyList({Property("session_id", kPropertyTypeString, std::string(""))
                                  .SetMaxLength(128)}),
                [this](const PropertyList& p) { return Stop(p); });

    ESP_LOGI(TAG, "casting tools registered, service=%s", url.c_str());
}

ToolResult CastTools::ListScreens(const PropertyList&) {
    CastReply reply = client_.GetTargets();
    if (!reply.ok) {
        return std::unexpected(reply.message);
    }
    if (!RememberTargets(reply.body)) {
        return std::unexpected("投屏服务返回的内容无法解析");
    }
    if (targets_.empty()) {
        return std::string("附近没有发现可投屏的电视");
    }
    std::string result;
    for (const auto& entry : targets_) {
        if (!result.empty()) {
            result += "；";
        }
        result += Label(entry.second) + StatusWord(entry.second);
    }
    return result;
}

ToolResult CastTools::Cast(const PropertyList& properties) {
    std::string url = Trim(properties["url"].value<std::string>());
    std::string target_id = Trim(properties["target_id"].value<std::string>());
    if (url.empty() || target_id.empty()) {
        return std::unexpected("需要视频地址和电视编号");
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return std::unexpected("视频地址不合法");
    }
    auto target = Resolve(target_id);
    if (!target) {
        return std::unexpected(target.error());
    }
    if (target->endpoint_id.empty()) {
        return std::unexpected("这块电视不支持这种画面格式");
    }

    CJsonUniquePtr request(cJSON_CreateObject());
    CJsonUniquePtr source(cJSON_CreateObject());
    if (request == nullptr || source == nullptr) {
        return std::unexpected("内存不足");
    }
    cJSON_AddStringToObject(source.get(), "type", "stream");
    cJSON_AddStringToObject(source.get(), "url", url.c_str());
    cJSON_AddStringToObject(source.get(), "format", kFormat);
    cJSON_AddStringToObject(source.get(), "video_codec", kVideoCodec);
    cJSON_AddStringToObject(source.get(), "audio_codec", kAudioCodec);
    cJSON_AddStringToObject(request.get(), "target_id", target->id.c_str());
    cJSON_AddStringToObject(request.get(), "endpoint_id", target->endpoint_id.c_str());
    cJSON_AddItemToObject(request.get(), "source", source.get());
    source.release();

    CastReply reply = client_.CreateSession(PrintJson(request.get()));
    if (!reply.ok) {
        return std::unexpected(reply.message);
    }
    CJsonUniquePtr session(cJSON_Parse(reply.body.c_str()));
    std::string session_id = StringField(session.get(), "session_id");
    if (session_id.empty()) {
        return std::unexpected("投屏服务没有返回投屏编号");
    }
    last_session_ = session_id;
    target->active_session_id = session_id;
    targets_[target->id] = *target;

    const std::string label = Label(*target);
    if (StringField(session.get(), "state") == "playing") {
        return "已经投到" + label + "了";
    }
    return "已经开始投到" + label + "，电视正在连接";
}

ToolResult CastTools::Stop(const PropertyList& properties) {
    std::string session_id = Trim(properties["session_id"].value<std::string>());
    if (session_id.empty()) {
        session_id = last_session_;
    }
    if (session_id.empty()) {
        return std::unexpected("现在没有正在投屏的内容");
    }
    CastReply reply = client_.StopSession(session_id);
    if (!reply.ok) {
        // A session the service no longer knows about is already stopped, and
        // saying so ends the conversation instead of inviting a retry.
        if (reply.status != 404) {
            return std::unexpected(reply.message);
        }
        if (session_id == last_session_) {
            last_session_.clear();
        }
        return std::string("这块电视已经不在投屏了");
    }
    if (session_id == last_session_) {
        last_session_.clear();
    }
    for (auto& entry : targets_) {
        if (entry.second.active_session_id == session_id) {
            entry.second.active_session_id.clear();
        }
    }
    return std::string("已经停止投屏");
}

bool CastTools::ReadTarget(const cJSON* item, Target& target) const {
    target.id = StringField(item, "id");
    if (target.id.empty()) {
        return false;
    }
    target.name = StringField(item, "name");
    target.state = StringField(item, "state");
    target.active_session_id = StringField(item, "active_session_id");
    target.endpoint_id.clear();
    const cJSON* endpoints = cJSON_GetObjectItem(item, "endpoints");
    if (cJSON_IsArray(endpoints)) {
        for (const cJSON* endpoint = endpoints->child; endpoint != nullptr;
             endpoint = endpoint->next) {
            if (!SupportsStream(endpoint)) {
                continue;
            }
            target.endpoint_id = StringField(endpoint, "id");
            break;
        }
    }
    return true;
}

bool CastTools::RememberTargets(const std::string& body) {
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    const cJSON* targets = cJSON_GetObjectItem(root.get(), "targets");
    if (!cJSON_IsArray(targets)) {
        return false;
    }
    std::map<std::string, Target> parsed;
    for (const cJSON* item = targets->child; item != nullptr; item = item->next) {
        Target target;
        if (ReadTarget(item, target)) {
            parsed[target.id] = target;
        }
    }
    targets_ = std::move(parsed);
    return true;
}

std::expected<CastTools::Target, std::string> CastTools::Resolve(
    const std::string& target_id) {
    auto cached = targets_.find(target_id);
    if (cached != targets_.end()) {
        return cached->second;
    }
    // The model often repeats the name it just heard instead of the id that
    // list_screens returned. A unique cached name is enough to cast.
    const Target* named = nullptr;
    int name_hits = 0;
    for (const auto& entry : targets_) {
        if (entry.second.name != target_id) {
            continue;
        }
        named = &entry.second;
        ++name_hits;
    }
    if (name_hits == 1 && named != nullptr && !named->endpoint_id.empty()) {
        return *named;
    }
    if (name_hits > 1) {
        return std::unexpected("附近有好几块叫这个名字的电视，先调用 self.cast.list_screens 确认是哪一块");
    }
    // The model may cast without listing first, or the cache may be stale.
    CastReply reply = client_.GetTarget(target_id);
    if (!reply.ok) {
        if (reply.status == 404) {
            return std::unexpected("找不到这块电视，请先调用 self.cast.list_screens 看看有哪些");
        }
        return std::unexpected(reply.message);
    }
    Target target;
    CJsonUniquePtr parsed(cJSON_Parse(reply.body.c_str()));
    if (!ReadTarget(parsed.get(), target) || target.id != target_id) {
        return std::unexpected("投屏服务返回的内容无法解析");
    }
    targets_[target.id] = target;
    return target;
}

std::string CastTools::Label(const Target& target) const {
    if (!target.name.empty()) {
        return target.name;
    }
    if (target.id.size() <= 6) {
        return target.id;
    }
    return target.id.substr(target.id.size() - 6);
}

std::string CastTools::StatusWord(const Target& target) const {
    if (target.state == "offline") {
        return "离线";
    }
    if (!target.active_session_id.empty()) {
        return "正在播放";
    }
    return "空闲";
}
