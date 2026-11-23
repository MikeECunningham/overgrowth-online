#include "campaign_scale.h"
#include <Online/online.h>

namespace OnlineMessages {
CampaignScale::CampaignScale() : OnlineMessageBase(OnlineMessageCategory::LEVEL_PERSISTENT) {}
CampaignScale::CampaignScale(CommonObjectID character, float vitality, float damage, float armor, float ai)
    : OnlineMessageBase(OnlineMessageCategory::LEVEL_PERSISTENT), character(character), vitality(vitality), damage(damage), armor(armor), ai(ai) {}
binn* CampaignScale::Serialize(void* object) {
    auto* m = static_cast<CampaignScale*>(object);
    binn* data = binn_object();
    binn_object_set_int32(data, "c", m->character);
    binn_object_set_float(data, "v", m->vitality);
    binn_object_set_float(data, "d", m->damage);
    binn_object_set_float(data, "r", m->armor);
    binn_object_set_float(data, "a", m->ai);
    return data;
}
void CampaignScale::Deserialize(void* object, binn* data) {
    auto* m = static_cast<CampaignScale*>(object);
    binn_object_get_int32(data, "c", &m->character);
    binn_object_get_float(data, "v", &m->vitality);
    binn_object_get_float(data, "d", &m->damage);
    binn_object_get_float(data, "r", &m->armor);
    binn_object_get_float(data, "a", &m->ai);
}
void CampaignScale::Execute(const OnlineMessageRef&, void* object, PeerID) {
    auto* m = static_cast<CampaignScale*>(object);
    Online::Instance()->ReceiveCampaignScale(m->character, m->vitality, m->damage, m->armor, m->ai);
}
void* CampaignScale::Construct(void* memory) { return new (memory) CampaignScale(); }
void CampaignScale::Destroy(void* object) { static_cast<CampaignScale*>(object)->~CampaignScale(); }
}
