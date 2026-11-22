#pragma once
#include "online_message_base.h"

namespace OnlineMessages {
// Reliable session control, deliberately independent of loaded level state.
class LobbyControl : public OnlineMessageBase {
 public:
    std::string payload;
    explicit LobbyControl(const std::string& payload);
    static binn* Serialize(void* object);
    static void Deserialize(void* object, binn* source);
    static void Execute(const OnlineMessageRef&, void* object, PeerID from);
    static void* Construct(void* mem);
    static void Destroy(void* object);
};
}
