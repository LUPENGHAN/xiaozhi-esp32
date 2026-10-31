#ifndef CAST_CLIENT_H
#define CAST_CLIENT_H

#include <string>

// One call's outcome. ok=false means the local casting service could not be
// reached or refused the request; message is worded so the model can read it
// back to the user. body holds the JSON of a successful response.
struct CastReply {
    bool ok = false;
    std::string body;
    std::string message;
    int status = 0;
    // The service's error code, for example target_busy or target_offline.
    std::string code;
};

// CastClient talks to the casting service running on the site LAN. The device
// never builds an application and never touches the cloud stream itself: the
// control plane starts the app and returns a stream URL, which the television
// pulls directly from the cloud.
//
// The casting service has no authentication, so this client sends no identity
// headers or tokens and the URL must stay on a trusted network.
class CastClient {
public:
    void Configure(const std::string& base_url);
    bool Configured() const { return !base_url_.empty(); }

    // GetTargets lists the screens the service can reach right now.
    CastReply GetTargets();
    // GetTarget refreshes one screen, by id.
    CastReply GetTarget(const std::string& target_id);
    // CreateSession asks the service to put a stream on a screen.
    CastReply CreateSession(const std::string& request_json);
    // StopSession stops projection and releases the screen.
    CastReply StopSession(const std::string& session_id);

private:
    CastReply Request(const std::string& method, const std::string& path,
                      const std::string& body, int timeout_ms);

    std::string base_url_;
};

#endif  // CAST_CLIENT_H
