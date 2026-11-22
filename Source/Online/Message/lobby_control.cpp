#include "lobby_control.h"
#include <Online/online.h>
#include <Utility/binn_util.h>

namespace OnlineMessages {
LobbyControl::LobbyControl(const std::string& text) : OnlineMessageBase(OnlineMessageCategory::TRANSIENT), payload(text) {}
binn* LobbyControl::Serialize(void* object) {
    binn* value = binn_object();
    binn_object_set_std_string(value, "lobby", static_cast<LobbyControl*>(object)->payload);
    return value;
}
void LobbyControl::Deserialize(void* object, binn* value) {
    binn_object_get_std_string(value, "lobby", &static_cast<LobbyControl*>(object)->payload);
}
void LobbyControl::Execute(const OnlineMessageRef&, void* object, PeerID from) {
    const auto& payload = static_cast<LobbyControl*>(object)->payload;
    if (payload.size() <= (1u << 20)) Online::Instance()->ReceiveLobbyControl(from, payload);
}
void* LobbyControl::Construct(void* mem) { return new (mem) LobbyControl(""); }
void LobbyControl::Destroy(void* object) { static_cast<LobbyControl*>(object)->~LobbyControl(); }
}
