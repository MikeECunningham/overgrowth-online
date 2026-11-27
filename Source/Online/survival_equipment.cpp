// Physical weapon recovery between Survival batteries. Apache-2.0.
#include <Online/survival_equipment.h>
#include <Online/online.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Objects/itemobject.h>
#include <Objects/movementobject.h>

#include <algorithm>

namespace SurvivalEquipment {
namespace {
MovementObject* DefeatedWaveCarrier(SceneGraph* graph, int id) {
    Object* object = id >= 0 ? graph->GetObjectFromID(id) : nullptr;
    if (!object || object->GetType() != _movement_object || !object->GetScriptParams()->HasParam("Survival Team")) return nullptr;
    auto* actor = static_cast<MovementObject*>(object);
    return actor->as_context && actor->ASGetIntVar("knocked_out") != 0 ? actor : nullptr;
}

bool RecallSlot(SceneGraph* graph, MovementObject& actor, ItemObject& item, AttachmentSlot& result) {
    if (!actor.ASHasVar("weapon_slots") || !actor.HasFunction("void HandleEditorAttachment(int, int, bool)")) return false;
    int hands_free = 2;
    bool occupied[4];
    for (int index = 0; index < 4; ++index) {
        const int id = actor.ASGetArrayIntVar("weapon_slots", index);
        Object* held = id >= 0 ? graph->GetObjectFromID(id) : nullptr;
        occupied[index] = held && held->GetType() == _item_object;
        if (occupied[index] && index < 2)
            hands_free -= static_cast<ItemObject*>(held)->item_ref()->GetNumHands();
    }
    // Include attachments still waiting for the owner's next script snapshot.
    for (const auto& attached : actor.rigged_object()->attached_items.items) {
        const auto& reader = attached.item;
        if (!reader.GetAttached()) continue;
        if (reader.attachment_type == _at_grip) {
            int index = reader.attachment_mirror ? 0 : 1;
            if (!occupied[index]) hands_free -= reader.GetAttached()->item_ref()->GetNumHands();
            occupied[index] = true;
        } else if (reader.attachment_type == _at_sheathe) {
            occupied[reader.attachment_mirror ? 3 : 2] = true;
        }
    }
    AttachmentSlotList slots;
    actor.rigged_object()->AvailableItemSlots(item.item_ref(), &slots);
    for (auto type : {_at_sheathe, _at_grip}) {
        if (type == _at_grip && item.item_ref()->GetNumHands() > hands_free) continue;
        for (const auto& slot : slots) {
            int index = type == _at_grip ? (slot.mirrored ? 0 : 1) : (slot.mirrored ? 3 : 2);
            if (slot.type == type && !occupied[index]) { result = slot; return true; }
        }
    }
    return false;
}

void Stow(MovementObject& actor, ItemObject& item, const AttachmentSlot& slot) {
    const int id = item.GetID();
    item.SetTranslation(actor.position);
    item.SetParent(&actor);
    actor.AttachItemToSlot(id, slot.type, slot.mirrored);
    actor.Execute("HandleEditorAttachment(" + std::to_string(id) + "," + std::to_string(slot.type) + "," +
                  (slot.mirrored ? "true" : "false") + ");");
}
}  // namespace

void RecallDroppedWeapons() {
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    Online* net = Online::Instance();
    if (!net->IsHosting() || !net->IsSurvivalMode() || !graph) return;
    const auto roster = net->GetPlayerStates();
    for (auto* object : graph->item_objects_) {
        auto* item = static_cast<ItemObject*>(object);
        if (!item->enabled_ || !item->item_ref().valid() || item->item_ref()->GetItemType() != _weapon) continue;
        auto* params = item->GetScriptParams();
        PlayerState last;
        PlayerID owner;
        if (params->HasParam("Survival Recall Slot")) owner = static_cast<PlayerID>(params->ASGetInt("Survival Recall Slot"));
        else if (item->last_held_char_id_ >= 0 && net->TryGetPlayerState(last, item->last_held_char_id_) && !last.is_bot) owner = last.player_id;
        else continue;
        const auto seat = std::find_if(roster.begin(), roster.end(), [owner](const PlayerState& p) {
            return p.player_id == owner && !p.is_bot && p.connected && p.object_id >= 0;
        });
        if (seat == roster.end()) continue;
        // Query current physical ownership after the finishing camera. A dead
        // raider may still have a sheathed item even after dropping its hands.
        int holder = item->HeldByWhom(), stuck = item->StuckInWhom();
        MovementObject* former = DefeatedWaveCarrier(graph, holder);
        MovementObject* corpse = DefeatedWaveCarrier(graph, stuck);
        if ((holder >= 0 && !former) || (stuck >= 0 && !corpse)) continue;
        Object* body = graph->GetObjectFromID(seat->object_id);
        if (!body || body->GetType() != _movement_object || !body->enabled_) continue;
        auto* actor = static_cast<MovementObject*>(body);
        AttachmentSlot slot;
        if (!actor->as_context || !actor->rigged_object() || !RecallSlot(graph, *actor, *item, slot)) continue;
        const int id = item->GetID();
        if (former) {
            former->NetworkDetachItem(id);
            former->Execute("NotifyItemDetach(" + std::to_string(id) + ");");
            net->Send<OnlineMessages::AttachToMessage>(holder, id, 0, false, false, nullptr, vec3(1), 0, true);
        }
        if (corpse) corpse->rigged_object()->UnStickItem(id);
        if (net->IsLocalAvatar(actor->GetID())) {
            Stow(*actor, *item, slot);
            net->Send<OnlineMessages::AttachToMessage>(actor->GetID(), id, slot.type, true, slot.mirrored, nullptr, vec3(1), 0, true);
        } else {
            // Its owner may have picked up something newer than our snapshot.
            // The owner chooses a legal socket and reports it in its usual inventory stream.
            net->Send<OnlineMessages::SendLevelMessage>("ohg recall " + std::to_string(net->GetOriginalID(actor->GetID())) +
                                                        " " + std::to_string(net->GetOriginalID(id)));
        }
    }
}

void ReceiveRecall(int character, int weapon) {
    Online* net = Online::Instance();
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (!graph || !net->IsClient() || !net->IsSurvivalMode()) return;
    const int who = net->GetObjectID(character);
    if (!net->IsLocalAvatar(who)) return;
    Object* body = graph->GetObjectFromID(who);
    Object* object = graph->GetObjectFromID(net->GetObjectID(weapon));
    if (!body || !body->enabled_ || body->GetType() != _movement_object ||
        !object || !object->enabled_ || object->GetType() != _item_object) return;
    auto* actor = static_cast<MovementObject*>(body);
    auto* item = static_cast<ItemObject*>(object);
    if (!actor->rigged_object() || !actor->as_context || !item->item_ref().valid() ||
        item->item_ref()->GetItemType() != _weapon || item->HeldByWhom() >= 0) return;
    // The host's unstick broadcast may still be queued behind this request.
    const int stuck = item->StuckInWhom();
    if (stuck >= 0 && !DefeatedWaveCarrier(graph, stuck)) return;
    AttachmentSlot slot;
    if (!RecallSlot(graph, *actor, *item, slot)) return;
    if (stuck >= 0) net->UnstickHere(graph, item);
    Stow(*actor, *item, slot);
}
}  // namespace SurvivalEquipment
