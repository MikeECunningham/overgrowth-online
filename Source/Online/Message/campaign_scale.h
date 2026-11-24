// Host-authoritative campaign combat factors. Apache-2.0.
#pragma once
#include "online_message_base.h"
#include <Online/online_datastructures.h>

namespace OnlineMessages {
class CampaignScale : public OnlineMessageBase {
public:
    CommonObjectID character = -1;
    float vitality = 1, damage = 1, armor = 1, ai = 0;
    int armor_regions = -1;
    CampaignScale();
    CampaignScale(CommonObjectID character, float vitality, float damage, float armor, float ai, int armor_regions = -1);
    static binn* Serialize(void* object);
    static void Deserialize(void* object, binn* data);
    static void Execute(const OnlineMessageRef& ref, void* object, PeerID peer);
    static void* Construct(void* memory);
    static void Destroy(void* object);
};
}
