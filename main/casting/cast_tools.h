#ifndef CAST_TOOLS_H
#define CAST_TOOLS_H

#include <expected>
#include <map>
#include <string>

#include "cast_client.h"
#include "mcp_server.h"

// CastTools registers the self.cast.* device tools. The device discovers the
// screens on its own LAN and asks the local casting service to play a stream
// address there; building and running the application is the control plane's
// job and happens before the device is involved.
//
// Tool callbacks run on the application task, so everything here is single
// threaded and the cached screen list needs no lock.
class CastTools {
public:
    static CastTools& GetInstance();

    // Initialize registers the tools. A build without a casting URL registers
    // nothing.
    void Initialize();

private:
    CastTools() = default;

    struct Target {
        std::string id;
        std::string name;
        std::string state;
        std::string active_session_id;
        // The endpoint that can play our stream profile; empty when none can.
        std::string endpoint_id;
    };

    ToolResult ListScreens(const PropertyList& properties);
    ToolResult Cast(const PropertyList& properties);
    ToolResult Stop(const PropertyList& properties);

    // ReadTarget fills target from one JSON object; false when it has no id.
    bool ReadTarget(const cJSON* item, Target& target) const;
    // Remember replaces the cache with the targets in a /v1/targets body.
    bool RememberTargets(const std::string& body);
    // Resolve returns a screen from the cache, refreshing it first when the id
    // has not been discovered yet.
    std::expected<Target, std::string> Resolve(const std::string& target_id);
    std::string Label(const Target& target) const;
    std::string StatusWord(const Target& target) const;

    CastClient client_;
    bool initialized_ = false;
    std::map<std::string, Target> targets_;
    // The session this device started most recently, so "stop" needs no
    // argument during a conversation.
    std::string last_session_;
};

#endif  // CAST_TOOLS_H
