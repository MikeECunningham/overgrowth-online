// Party-size scaling for authored campaign encounters. Apache-2.0.
#include <Online/campaign_difficulty.h>
#include <Online/challenge_scaling.h>
#include <Online/online.h>
#include <Online/online_utility.h>
#include <AI/navmesh.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Game/level.h>
#include <Game/EntityDescription.h>
#include <Editors/actors_editor.h>
#include <Editors/map_editor.h>
#include <Internal/config.h>
#include <Objects/hotspot.h>
#include <Objects/itemobject.h>
#include <Objects/movementobject.h>
#include <Objects/riggedobject.h>
#include <Physics/bulletworld.h>
#include <Logging/logdata.h>
#include <Scripting/angelscript/ascontext.h>
#include <Math/vec3math.h>
#include <Graphics/camera.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <array>
#include <set>
#include <sstream>

namespace {
struct Reinforcement {
    ObjectID actor = -1;
    std::vector<ObjectID> equipment;
};
struct Encounter {
    ObjectID hotspot = -1;
    int goal = 0;
    std::vector<ObjectID> originals;
    std::set<ObjectID> bosses;
    std::vector<Reinforcement> extras;
    std::set<PlayerID> opposing_seats;
    int spawned = 0;  // dead reinforcements are never replaced
    int reinforcement_target = 0;
    bool reinforcements_deferred = false;
    bool reinforcements_recheck = false;
};
MovementObject* Actor(SceneGraph* graph, ObjectID id) {
    Object* object = graph ? graph->GetObjectFromID(id) : nullptr;
    return object && object->GetType() == _movement_object ? static_cast<MovementObject*>(object) : nullptr;
}
bool Awake(MovementObject* actor) {
    return actor && actor->ASHasVar("knocked_out") && actor->ASGetIntVar("knocked_out") == 0;
}
bool LobbyBot(Online* online, ObjectID id) {
    if (online->online_session) for (const auto& player : online->online_session->player_states)
        if (player.second.object_id == id) return player.second.is_bot;
    return false;
}
int Strength(MovementObject* actor) {
    if (!actor || !actor->ASHasVar("species")) return CampaignDifficulty::Rabbit;
    switch (actor->ASGetIntVar("species")) {
        case 1: return CampaignDifficulty::Wolf;
        case 2: return actor->character_path.find("dog_big") != std::string::npos ? CampaignDifficulty::BigDog : CampaignDifficulty::CatOrDog;
        case 3: return CampaignDifficulty::Rat;
        case 4: return CampaignDifficulty::CatOrDog;
        default: return CampaignDifficulty::Rabbit;
    }
}
bool OpposesMain(Online* online, SceneGraph* graph, MovementObject* actor) {
    if (!actor) return false;
    if (!online->CoopCampaign()) {
        MovementObject* reference = Actor(graph, online->ChallengeReferenceAvatar());
        return reference && !actor->ASOnSameTeam(reference);
    }
    bool member = false;
    for (const auto& player : online->GetPlayerStates()) {
        if (!online->CampaignPlayerOnMainTeam(player.player_id)) continue;
        MovementObject* avatar = Actor(graph, player.object_id);
        if (!avatar) continue;
        member = true;
        if (actor->ASOnSameTeam(avatar)) return false;
    }
    return member;
}
bool Hostile(Online* online, SceneGraph* graph, MovementObject* actor) {
    if (!actor || actor->controlled || (online->IsPlayerCharacter(actor->GetID()) && !LobbyBot(online, actor->GetID())) ||
        !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) return false;
    ScriptParams* params = actor->GetScriptParams();
    if (params->HasParam("Campaign Scaling Exempt") && params->ASGetInt("Campaign Scaling Exempt") != 0) return false;
    return OpposesMain(online, graph, actor);
}
bool ExplicitBoss(MovementObject* actor, bool inferred) {
    ScriptParams* params = actor->GetScriptParams();
    return params->HasParam("Campaign Boss") ? params->ASGetInt("Campaign Boss") != 0 : inferred;
}
bool Elite(MovementObject* actor) {
    ScriptParams* params = actor->GetScriptParams();
    // Ordinary story guards also have large scripted knockout shields. That
    // protection alone does not make an encounter a boss fight.
    return params->HasParam("Damage Resistance") && params->ASGetFloat("Damage Resistance") >= 1.5f;
}
float NearestMainParty(Online* online, SceneGraph* graph, const vec3& position, bool humans_only = false) {
    float nearest = 1e30f;
    for (const auto& player : online->GetPlayerStates()) {
        if (!online->CampaignPlayerOnMainTeam(player.player_id) || (humans_only && player.is_bot)) continue;
        if (MovementObject* actor = Actor(graph, player.object_id))
            if (actor->enabled_ && Awake(actor)) nearest = std::min(nearest, distance(actor->position, position));
    }
    return nearest;
}
int MainPartyBots(const Online* online, SceneGraph* graph) {
    int bots = 0;
    if (online->online_session) for (const auto& player : online->online_session->player_states)
        if (player.second.is_bot && online->CampaignPlayerOnMainTeam(player.first) && Actor(graph, player.second.object_id)) ++bots;
    return bots;
}
int EncounterReinforcements(SceneGraph* graph, const Encounter& encounter, int humans, int bots) {
    int rank = 0;
    for (ObjectID id : encounter.originals) if (MovementObject* actor = Actor(graph, id)) rank = std::max(rank, Strength(actor));
    return CampaignDifficulty::ExtraEnemiesForParty(humans, bots, static_cast<int>(encounter.originals.size()), !encounter.bosses.empty(), rank);
}
bool ReadyForReinforcements(MovementObject* actor) {
    return actor && actor->enabled_ && !actor->static_char &&
        !(actor->ASHasVar("dialogue_control") && actor->ASGetBoolVar("dialogue_control"));
}
void RemoveReinforcement(SceneGraph* graph, const Reinforcement& extra) {
    for (ObjectID id : extra.equipment) {
        Object* object = graph->GetObjectFromID(id);
        if (object && object->GetType() == _item_object) {
            const int holder = static_cast<ItemObject*>(object)->HeldByWhom();
            // Loot taken by another character belongs to that character now.
            if (holder == -1 || holder == extra.actor) graph->map_editor->DeleteID(id);
        }
    }
    if (Actor(graph, extra.actor)) graph->map_editor->DeleteID(extra.actor);
}
bool ReinforcementPosition(Online* online, SceneGraph* graph, MovementObject* source, int index, vec3* position) {
    if (!graph->bullet_world_) return false;
    const vec3 up(0, 1, 0);
    NavMesh* navmesh = graph->GetNavMesh();
    NavPoint source_nav;
    if (navmesh) source_nav = navmesh->GetNavPoint(source->position);
    const bool on_navmesh = source_nav.IsSuccess() && distance_squared(source_nav.GetPoint(), source->position) < 4.0f;
    // FindSafeSpot returns the first world-safe candidate, even if another
    // character occupies it. Rotating its index alone keeps returning the same
    // eight inner-ring positions. Alternate bases also reach the outer space.
    for (int attempt = 0; attempt < 32; ++attempt) {
        vec3 base = source->position;
        if (attempt >= 8) {
            const float radius = 1.8f + 1.4f * ((attempt - 8) / 8);
            const float angle = ((attempt + index) % 8) * 0.7853981634f;
            base += vec3(std::cos(angle) * radius, 0, std::sin(angle) * radius);
        }
        vec3 candidate;
        if (!online->FindSafeSpot(base, source->GetFacing(), attempt < 8 ? index + attempt + 1 : 0,
                                  &candidate, source->GetID())) continue;
        // Alternate bases must remain on this side of walls and ledges, not
        // find an unrelated safe floor through a wall or on the storey below.
        if (std::fabs(candidate.y() - source->position.y()) > 0.6f ||
            graph->bullet_world_->CheckRayCollision(source->position, candidate) ||
            graph->bullet_world_->CheckRayCollision(source->position + up * 0.6f, candidate + up * 0.6f)) continue;
        bool occupied = false;
        for (Object* object : graph->movement_objects_) {
            const auto* actor = static_cast<MovementObject*>(object);
            if (!actor->enabled_) continue;
            const vec3 offset = actor->position - candidate;
            if (std::fabs(offset.y()) < 1.8f && offset.x() * offset.x() + offset.z() * offset.z() < 1.4f) { occupied = true; break; }
        }
        if (occupied) continue;
        if (on_navmesh) {
            NavPoint candidate_nav = navmesh->GetNavPoint(candidate);
            if (!candidate_nav.IsSuccess() || distance_squared(candidate_nav.GetPoint(), candidate) >= 4.0f ||
                !navmesh->FindPath(source_nav.GetPoint(), candidate_nav.GetPoint(), 0xffff, 0).success) continue;
        }
        *position = candidate;
        return true;
    }
    return false;
}
void CopyParameter(Online* online, ScriptParams* dest, const std::string& key, const ScriptParam& param) {
    dest->InsertScriptParam(key, param);
    if (param.IsString()) online->Send<OnlineMessages::SPStringMessage>(dest->GetObjectID(), key, param.GetString(), param.editor().type(), param.editor().GetDetails());
    else if (param.IsInt() || param.IsFloat()) online->SendIntFloatScriptParam(dest->GetObjectID(), key, param);
}
Reinforcement Spawn(Online* online, SceneGraph* graph, MovementObject* source, int index) {
    Reinforcement result;
    if (!OnlineUtility::IsSafeAssetPath(source->obj_file) || !OnlineUtility::IsSafeCharacterPath(source->character_path)) return result;
    vec3 position;
    if (!ReinforcementPosition(online, graph, source, index, &position)) return result;
    EntityDescriptionList descriptions;
    std::string type;
    Path path;
    ActorsEditor_LoadEntitiesFromFile(source->obj_file, descriptions, &type, &path);
    for (auto& desc : descriptions) {
        if (GetTypeFromDesc(desc) != _movement_object) continue;
        // Fresh actor template: no authored waypoint, hotspot, inventory or
        // editor-child references may be duplicated from the story actor.
        desc.children.clear();
        desc.fields.erase(std::remove_if(desc.fields.begin(), desc.fields.end(), [](const EntityDescriptionField& field) {
            return field.type == EDF_TRANSLATION || field.type == EDF_IS_PLAYER || field.type == EDF_CONNECTIONS ||
                   field.type == EDF_CONNECTIONS_FROM || field.type == EDF_ITEM_CONNECTIONS || field.type == EDF_ENV_OBJECT_ATTACH;
        }), desc.fields.end());
        desc.AddVec3(EDF_TRANSLATION, position);
        desc.AddBool(EDF_IS_PLAYER, false);
        Object* object = MapEditor::AddEntityFromDesc(graph, desc, false);
        auto* actor = object && object->GetType() == _movement_object ? static_cast<MovementObject*>(object) : nullptr;
        if (!actor) return result;
        result.actor = actor->GetID();
        actor->is_player = false; actor->controlled = false; actor->controller_id = -1; actor->camera_id = 0;
        actor->created_on_the_fly = true; actor->exclude_from_save = true; actor->exclude_from_undo = true;
        actor->SetRotation(source->GetRotation());
        actor->ChangeControlScript(graph->level->GetNPCScript(actor));
        online->Send<OnlineMessages::CreateEntity>(source->obj_file, position, actor->GetID());
        actor->ApplyNetworkCharacter(source->character_path);
        online->SendAvatarCharacter(actor->GetID(), source->character_path);
        const char* keys[] = {"Teams", "Aggression", "Damage Resistance", "Block Skill", "Block Follow-up", "Ground Aggression",
            "Attack Speed", "Attack Damage", "Attack Knockback", "Movement Speed", "Character Scale", "Knockout Shield",
            "Left handed", "Fat", "Muscle", "Ear Size", "Wearing Metal Armor", "Weapon Catch Skill", "Species",
            "Throw Counter Probability", "Throw Trainer", "Cannot Be Disarmed", "No Look Around", "Stick To Nav Mesh",
            "Fear - Causes Fear On Sight", "Fear - Always Afraid On Sight", "Fear - Never Afraid On Sight", "Fear - Afraid At Health Level",
            "Jump - Initial Velocity", "Jump - Air Control", "Jump - Jump Sustain", "Jump - Jump Sustain Boost",
            "Focus FOV horizontal", "Focus FOV vertical", "Focus FOV distance", "Peripheral FOV horizontal",
            "Peripheral FOV vertical", "Peripheral FOV distance", "Fall Damage Multiplier", "Unarmed Stance Override"};
        for (const char* key : keys) {
            const auto& params = source->GetScriptParams()->GetParameterMap();
            const auto it = params.find(key);
            if (it != params.end()) CopyParameter(online, actor->GetScriptParams(), key, it->second);
        }
        actor->GetScriptParams()->ASSetInt("Campaign Scaling Source", online->GetOriginalID(source->GetID()));
        actor->GetScriptParams()->ASSetInt("Campaign Boss", 0);
        actor->GetScriptParams()->ASAddIntCheckbox("No Save", true);
        actor->UpdateScriptParams();
        actor->ApplyPalette(*source->GetPalette());
        std::set<ObjectID> copied;
        for (int slot = 0; slot < 6; ++slot) {
            const int original = source->ASGetArrayIntVar("weapon_slots", slot);
            Object* item_object = original >= 0 ? graph->GetObjectFromID(original) : nullptr;
            if (!item_object || item_object->GetType() != _item_object || !copied.insert(original).second) continue;
            if (!OnlineUtility::IsSafeAssetPath(item_object->obj_file)) continue;
            const int id = graph->map_editor->CreateObject(item_object->obj_file);
            Object* created = graph->GetObjectFromID(id);
            if (!created || created->GetType() != _item_object) continue;
            created->SetTranslation(position); created->exclude_from_save = true; created->exclude_from_undo = true;
            online->Send<OnlineMessages::CreateEntity>(item_object->obj_file, position, id);
            result.equipment.push_back(id);
            ItemObject* item = static_cast<ItemObject*>(created);
            AttachmentSlotList slots;
            actor->rigged_object()->AvailableItemSlots(item->item_ref(), &slots);
            const AttachmentType attachment = slot < 2 ? _at_grip : _at_sheathe;
            const bool mirror = slot == 0 || slot == 3 || slot == 5;
            for (const auto& candidate : slots) if (candidate.type == attachment && candidate.mirrored == mirror) {
                actor->AttachItemToSlotEditor(id, attachment, mirror, candidate.attachment_ref);
                break;
            }
        }
        // Join an ongoing fight without revealing an unaware patrol's targets.
        if (source->ASHasVar("target_id")) {
            const ObjectID target = source->ASGetIntVar("target_id");
            if (Awake(Actor(graph, target))) actor->ReceiveScriptMessage("notice " + std::to_string(target));
        }
        return result;
    }
    return result;
}
}

struct CampaignDifficultyState {
    struct Opponent {
        ObjectID avatar = -1;
        int team = 0;
        uint64_t deployment = 0;
        double notice_until = 0;
        ObjectID target = -1;
        bool placement_pending = false;
        vec3 pending_position;
        vec3 pending_facing;
        double queued_at = 0;
    };
    SceneGraph* graph = nullptr;
    std::map<std::pair<ObjectID, int>, Encounter> encounters;
    std::map<ObjectID, std::array<float, 4>> applied;
    bool reset = false;
    double next_update = 0;
    std::map<PlayerID, Opponent> opponents;
    std::set<std::pair<ObjectID, int>> engaged;
    uint64_t deployment = 0;
    ObjectID deployment_target = -1;
    std::set<PlayerID> deployment_seats;
    bool combat = false;
    double last_combat = -100000;
    double last_deployment = -100000;
    std::map<ObjectID, double> member_combat;
    std::pair<ObjectID, int> active_encounter = {-1, -1};
};

namespace {
void RecheckReinforcementGoals(Online* online, SceneGraph* graph, CampaignDifficultyState& state) {
    if (online->CoopCutscene() || graph->level->DialogueCameraControl()) return;
    for (auto& entry : state.encounters) {
        Encounter& encounter = entry.second;
        if (!encounter.reinforcements_recheck) continue;
        Object* object = graph->GetObjectFromID(encounter.hotspot);
        auto* hotspot = object && object->GetType() == _hotspot_object ? static_cast<Hotspot*>(object) : nullptr;
        if (!hotspot || hotspot->ASGetIntVar("progress") != encounter.goal) {
            encounter.reinforcements_recheck = false;
            continue;
        }
        if (!online->CampaignEncounterDefeated(encounter.hotspot, encounter.goal)) continue;
        encounter.reinforcements_recheck = false;
        // A cancellation or settings change need not produce another KO
        // event. Retry just this goal once, without fabricating combat events.
        hotspot->QueueScriptMessage("coop_recheck_encounter " + std::to_string(encounter.goal));
    }
}
void EnableSeat(Online* online, MovementObject* actor, bool enabled) {
    static_cast<Object*>(actor)->SetEnabled(enabled);  // authoritative native state, including physics and replication
    for (const auto& attachment : actor->rigged_object()->attached_items.items) {
        ItemObject* item = attachment.item.GetAttached();
        if (item && item->enabled_ != enabled) {
            item->SetEnabled(enabled);
            online->Send<OnlineMessages::SetObjectEnabledMessage>(item->GetID(), enabled);
        }
    }
}
void StageOpponents(Online* online, SceneGraph* graph, CampaignDifficultyState& state) {
    std::set<PlayerID> present;
    for (const auto& player : online->GetPlayerStates()) {
        MovementObject* actor = Actor(graph, player.object_id);
        if (!actor) continue;
        if (!online->LobbyForceTeams() || online->CampaignPlayerOnMainTeam(player.player_id)) {
            if (state.opponents.erase(player.player_id)) EnableSeat(online, actor, true);
            continue;
        }
        present.insert(player.player_id);
        auto& opponent = state.opponents[player.player_id];
        if (opponent.avatar != actor->GetID() || opponent.team != player.team) {
            opponent = CampaignDifficultyState::Opponent();
            opponent.avatar = actor->GetID(); opponent.team = player.team;
        }
        // A seat can be disabled before its first ordinary Update establishes
        // ownership. Keep remote humans as proxies even while unlinked.
        if (!actor->enabled_ || opponent.deployment == 0)
            actor->SetNetworkProxy(!online->IsLocalAvatar(actor->GetID()));
        if (opponent.deployment == 0) EnableSeat(online, actor, false);
        if (opponent.placement_pending) {
            vec3 observed = actor->position;
            if (!online->IsLocalAvatar(actor->GetID())) {
                uint32_t oldest = 0, newest = 0;
                if (actor->net_buffer.SourceTimeRange(oldest, newest)) actor->net_buffer.PositionAt(newest, observed);
            }
            if (Awake(actor) && distance_squared(observed, opponent.pending_position) < 2.25f) {
                EnableSeat(online, actor, true);
                opponent.placement_pending = false;
                opponent.notice_until = OnlineClockMs() + 1500.0;
            } else if (OnlineClockMs() - opponent.queued_at > 5000.0) {
                online->PlaceAvatar(actor->GetID(), opponent.pending_position, opponent.pending_facing, opponent.target);
                opponent.queued_at = OnlineClockMs();
            }
        }
    }
    for (auto it = state.opponents.begin(); it != state.opponents.end();)
        if (!present.count(it->first)) it = state.opponents.erase(it); else ++it;
}
std::vector<ObjectID> CombatParticipants(Online* online, SceneGraph* graph) {
    std::vector<ObjectID> result;
    for (const auto& player : online->GetPlayerStates()) {
        if (!online->CampaignPlayerOnMainTeam(player.player_id)) continue;
        MovementObject* member = Actor(graph, player.object_id);
        if (!Awake(member) || !member->enabled_) continue;
        for (Object* object : graph->movement_objects_) {
            auto* foe = static_cast<MovementObject*>(object);
            if (foe == member || !foe->enabled_ || !Awake(foe) || member->ASOnSameTeam(foe) ||
                distance_squared(member->position, foe->position) > 35.0f * 35.0f) continue;
            const bool foe_allowed = foe->controlled || !foe->ASHasVar("combat_allowed") || foe->ASGetBoolVar("combat_allowed");
            const bool member_allowed = member->controlled || !member->ASHasVar("combat_allowed") || member->ASGetBoolVar("combat_allowed");
            const bool member_attacks = member->ASHasVar("state") && member->ASGetIntVar("state") == 2 &&
                member->ASHasVar("target_id") && member->ASGetIntVar("target_id") == foe->GetID();
            const bool foe_attacks = foe_allowed && foe->ASHasVar("state") && foe->ASGetIntVar("state") == 2 &&
                foe->ASHasVar("target_id") && foe->ASGetIntVar("target_id") == member->GetID();
            const bool ai_chases = foe_allowed && !foe->controlled && foe->ASHasVar("goal") && foe->ASGetIntVar("goal") == 1 &&
                foe->ASHasVar("chase_target_id") && foe->ASGetIntVar("chase_target_id") == member->GetID();
            const bool member_chases = member_allowed && player.is_bot && member->ASHasVar("goal") && member->ASGetIntVar("goal") == 1 &&
                member->ASHasVar("chase_target_id") && member->ASGetIntVar("chase_target_id") == foe->GetID();
            if (member_attacks || foe_attacks || ai_chases || member_chases) { result.push_back(member->GetID()); break; }
        }
    }
    return result;
}
bool OpponentSpot(Online* online, SceneGraph* graph, MovementObject* member, unsigned seat, vec3* spot) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        const float angle = 0.618034f * static_cast<float>(seat) * 6.283185f + attempt * 0.785398f;
        const bool close = attempt >= 24;
        const float radius = close ? (attempt < 32 ? 4.0f : 2.5f) : 11.0f + static_cast<float>(attempt / 8) * 4.0f;
        const vec3 base = member->position + vec3(std::cos(angle) * radius, 0, std::sin(angle) * radius);
        if (!online->FindSafeSpot(base, member->GetFacing(), 0, spot, member->GetID())) continue;
        if (distance_squared(*spot, member->position) < (close ? 4.0f : 36.0f)) continue;
        bool occupied = false;
        for (Object* object : graph->movement_objects_) {
            auto* actor = static_cast<MovementObject*>(object);
            const vec3 offset = actor->position - *spot;
            if (actor->enabled_ && std::fabs(offset.y()) < 1.8f && length_squared(offset) < 3.0f) { occupied = true; break; }
        }
        if (!occupied) return true;
    }
    return false;
}
void DeployOpponents(Online* online, SceneGraph* graph, CampaignDifficultyState& state, double now) {
    if (!online->LobbyForceTeams() || online->CoopCutscene() || graph->level->DialogueCameraControl()) return;
    const auto participants = CombatParticipants(online, graph);
    if (participants.empty()) {
        if (now - state.last_combat >= 8000.0) state.combat = false;
    }
    ObjectID trigger = participants.empty() ? state.deployment_target : participants.front();
    bool entering = false;
    for (ObjectID id : participants) {
        const auto previous = state.member_combat.find(id);
        if (previous == state.member_combat.end() || now - previous->second >= 8000.0) {
            trigger = id; entering = true;
        }
        state.member_combat[id] = now;
    }
    MovementObject* member = Actor(graph, trigger);
    if (participants.empty() && (!Awake(member) || !member->enabled_)) {
        for (const auto& player : online->GetPlayerStates()) {
            if (!online->CampaignPlayerOnMainTeam(player.player_id)) continue;
            MovementObject* survivor = Actor(graph, player.object_id);
            if (Awake(survivor) && survivor->enabled_) {
                member = survivor; trigger = survivor->GetID(); break;
            }
        }
    }
    if (!Awake(member) || !member->enabled_) return;
    if (!participants.empty()) { state.combat = true; state.last_combat = now; }
    Encounter* encounter = nullptr;
    float closest = 1e30f;
    for (auto& entry : state.encounters) {
        Object* object = graph->GetObjectFromID(entry.first.first);
        if (!object || object->GetType() != _hotspot_object) continue;
        auto* hotspot = static_cast<Hotspot*>(object);
        if (hotspot->ASGetIntVar("progress") != entry.first.second) continue;
        for (ObjectID id : entry.second.originals) {
            MovementObject* source = Actor(graph, id);
            if (!source || !source->enabled_ || !Awake(source)) continue;
            const float d = distance_squared(source->position, member->position);
            if (d < closest && d < 55.0f * 55.0f) { closest = d; encounter = &entry.second; }
        }
    }
    if (encounter) state.active_encounter = {encounter->hotspot, encounter->goal};
    else {
        const auto old = state.encounters.find(state.active_encounter);
        Object* object = old != state.encounters.end() ? graph->GetObjectFromID(old->first.first) : nullptr;
        if (object && object->GetType() == _hotspot_object &&
            static_cast<Hotspot*>(object)->ASGetIntVar("progress") == old->first.second) encounter = &old->second;
    }
    const bool new_encounter = !participants.empty() && encounter && state.engaged.insert({encounter->hotspot, encounter->goal}).second;
    // Coalesce companions noticing the same fight over adjacent frames, while
    // allowing another MC member to start a new fight while P1 is still busy.
    if (new_encounter || (entering && now - state.last_deployment >= 2000.0)) {
        ++state.deployment; state.last_deployment = now;
        state.deployment_target = trigger;
        state.deployment_seats.clear();
    }
    if (!participants.empty()) {
        for (const auto& entry : state.opponents) {
            state.deployment_seats.insert(entry.first);
            if (encounter) encounter->opposing_seats.insert(entry.first);
        }
    }
    int attempted = 0;
    for (auto& entry : state.opponents) {
        auto& opponent = entry.second;
        MovementObject* actor = Actor(graph, opponent.avatar);
        if (!actor) continue;
        // Finish the captured event in bounded batches even if its authored
        // enemy dies before the remaining seats can be placed.
        if (state.deployment_seats.count(entry.first) && opponent.deployment != state.deployment && attempted < 4) {
            ++attempted;
            vec3 position;
            if (!OpponentSpot(online, graph, member, entry.first, &position)) {
                EnableSeat(online, actor, false);
                opponent.placement_pending = false;
                if (now - state.last_deployment >= 5000.0) {
                    // A crowded or tiny platform must not permanently lock
                    // the story. This seat retries at the next combat event.
                    opponent.deployment = state.deployment;
                    opponent.placement_pending = false;
                    for (auto& goal : state.encounters) goal.second.opposing_seats.erase(entry.first);
                    LOGI << "Campaign opposition: no safe ground for seat " << entry.first << ", deferred" << std::endl;
                }
                continue;
            }
            EnableSeat(online, actor, false);
            const CommonObjectID common = online->GetOriginalID(actor->GetID());
            if (online->owner_overrides.erase(common))
                online->BroadcastOwnerOverride(common, 0, false, 0, 0, false);
            actor->SetNetworkProxy(!online->IsLocalAvatar(actor->GetID()));
            vec3 facing = member->position - position; facing.y() = 0;
            if (length_squared(facing) > 0.01f) facing = normalize(facing);
            online->PlaceAvatar(actor->GetID(), position, facing, member->GetID());
            opponent.deployment = state.deployment;
            opponent.target = trigger; opponent.notice_until = 0;
            opponent.pending_position = position; opponent.pending_facing = facing;
            opponent.placement_pending = true; opponent.queued_at = now;
            if (encounter) encounter->opposing_seats.insert(entry.first);
            LOGI << "Campaign opposition: seat " << entry.first << " enters combat " << state.deployment << std::endl;
        }
        if (!actor->controlled && !opponent.placement_pending && now < opponent.notice_until &&
            (!actor->ASHasVar("goal") || actor->ASGetIntVar("goal") != 1))
            actor->ReceiveScriptMessage("notice " + std::to_string(opponent.target));
    }
}
}

int Online::CampaignHumanCount() const {
    int humans = 0;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (online_session) for (const auto& entry : online_session->player_states)
        if (!entry.second.is_bot && CampaignPlayerOnMainTeam(entry.first) && Actor(graph, entry.second.object_id)) ++humans;
    return std::max(1, humans);
}

int Online::CampaignPartySize() const {
    int participants = 0;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (online_session) for (const auto& entry : online_session->player_states)
        if (CampaignPlayerOnMainTeam(entry.first) && Actor(graph, entry.second.object_id)) ++participants;
    return std::max(1, participants);
}

bool Online::CampaignCharacterOnMainTeam(ObjectID avatar) {
    if (!IsActive() || !LobbyForceTeams() || !CoopCampaign()) return true;
    for (const auto& player : GetPlayerStates()) if (player.object_id == avatar) return CampaignPlayerOnMainTeam(player.player_id);
    return false;
}

std::string Online::CampaignWaitingText() {
    if (!IsActive() || !LobbyForceTeams() || !CoopCampaign() || CoopCutscene() || RemoteCutsceneActive()) return "";
    for (const auto& player : GetLocalPlayerStates()) {
        MovementObject* actor = Actor(Engine::Instance()->GetSceneGraph(), player.object_id);
        if (actor && player.camera_id == ActiveCameras::GetID() && !CampaignPlayerOnMainTeam(player.player_id) && !actor->enabled_)
            return "Waiting for the main character team to enter combat";
    }
    return "";
}

void Online::ReceiveCampaignScale(CommonObjectID character, float vitality, float damage, float armor, float ai) {
    if (!std::isfinite(vitality) || !std::isfinite(damage) || !std::isfinite(armor) || !std::isfinite(ai) ||
        vitality < 1 || vitality > 6 || damage < 1 || damage > 2 || armor < 1 || armor > 2 || ai < 0 || ai > 0.03f) return;
    MovementObject* actor = Actor(Engine::Instance()->GetSceneGraph(), GetObjectID(character));
    if (!actor || !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) return;
    if (IsPlayerCharacter(actor->GetID()) && !LobbyBot(this, actor->GetID()) && (vitality != 1 || damage != 1 || armor != 1 || ai != 0)) return;
    ASArglist args;
    args.Add(vitality); args.Add(damage); args.Add(armor); args.Add(ai);
    actor->as_context->CallScriptFunction("void SetCampaignScaling(float, float, float, float)", &args);
}

void Online::ResetCampaignDifficulty(bool scene_disposed) {
    if (scene_disposed) campaign_difficulty_state.reset();
    else if (IsHosting() && campaign_difficulty_state) campaign_difficulty_state->reset = true;
}

bool Online::CampaignEncounterDefeated(ObjectID hotspot, int goal) const {
    if (!IsHosting() || !campaign_difficulty_state || campaign_difficulty_state->reset) return true;
    const auto it = campaign_difficulty_state->encounters.find({hotspot, goal});
    if (it == campaign_difficulty_state->encounters.end()) return true;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    // A wave is allocated over several updates. Killing its authored enemies
    // must not advance the goal before the remaining reserved extras arrive.
    if (config["campaign_mp_scaling"].toBool() && !it->second.reinforcements_deferred &&
        it->second.spawned < std::min(it->second.reinforcement_target,
            EncounterReinforcements(graph, it->second, CampaignHumanCount(), MainPartyBots(this, graph)))) {
        it->second.reinforcements_recheck = true;
        return false;
    }
    for (const auto& extra : it->second.extras) if (Awake(Actor(graph, extra.actor))) return false;
    if (LobbyForceTeams() && online_session) for (PlayerID seat : it->second.opposing_seats) {
        const auto player = online_session->player_states.find(seat);
        if (player == online_session->player_states.end() || CampaignPlayerOnMainTeam(seat)) continue;
        MovementObject* actor = Actor(graph, player->second.object_id);
        const auto pending = campaign_difficulty_state->opponents.find(seat);
        if (actor && pending != campaign_difficulty_state->opponents.end() &&
            (pending->second.placement_pending || (campaign_difficulty_state->deployment_seats.count(seat) &&
             pending->second.deployment != campaign_difficulty_state->deployment))) return false;
        if (actor && actor->enabled_ && Awake(actor)) return false;
    }
    return true;
}

void Online::UpdateCampaignDifficulty(SceneGraph* graph) {
    if (!IsHosting() || !host_started_level || loading || LobbyWaiting() || LobbyBuild() || BuildSession() || !graph || !graph->level) return;
    const bool campaign = CoopCampaign();
    if (!campaign && !campaign_difficulty_state && ModeChallengePressure() <= 0) return;
    if (!campaign_difficulty_state || campaign_difficulty_state->graph != graph) {
        campaign_difficulty_state = std::make_shared<CampaignDifficultyState>();
        campaign_difficulty_state->graph = graph;
    }
    auto& state = *campaign_difficulty_state;
    const double now = OnlineClockMs();
    if (state.reset) {
        for (const auto& entry : state.encounters) for (const auto& extra : entry.second.extras) RemoveReinforcement(graph, extra);
        // A reset may also change allegiances. Restore every previous target
        // before rebuilding the roster, including a former enemy now allied.
        for (const auto& previous : state.applied) if (Actor(graph, previous.first)) {
            ReceiveCampaignScale(GetOriginalID(previous.first), 1, 1, 1, 0);
            Send<OnlineMessages::CampaignScale>(GetOriginalID(previous.first), 1, 1, 1, 0);
        }
        state.encounters.clear(); state.applied.clear(); state.reset = false; state.next_update = 0;
        state.opponents.clear(); state.engaged.clear(); state.deployment = 0; state.combat = false; state.last_combat = -100000;
        state.deployment_target = -1; state.deployment_seats.clear();
        state.member_combat.clear(); state.active_encounter = {-1, -1}; state.last_deployment = -100000;
    }
    if (campaign) StageOpponents(this, graph, state);
    const int humans = CampaignHumanCount();
    const int bots = MainPartyBots(this, graph);
    const bool stats = campaign && config["campaign_mp_scaling"].toBool();
    // Reserve a newly activated fight on the first ordinary update, even if
    // the heavier scaling/spawning pass is still on its 350ms throttle.
    if (stats && !coop_cutscene && !graph->level->DialogueCameraControl()) for (auto& entry : state.encounters) {
        Encounter& encounter = entry.second;
        Object* object = graph->GetObjectFromID(encounter.hotspot);
        if (encounter.reinforcements_deferred || !object || object->GetType() != _hotspot_object ||
            static_cast<Hotspot*>(object)->ASGetIntVar("progress") != encounter.goal) continue;
        for (ObjectID id : encounter.originals) {
            MovementObject* actor = Actor(graph, id);
            if (ReadyForReinforcements(actor) && Awake(actor) && NearestMainParty(this, graph, actor->position) < 55.0f) {
                encounter.reinforcement_target = EncounterReinforcements(graph, encounter, humans, bots);
                break;
            }
        }
    }
    if (now < state.next_update) return;
    state.next_update = now + 350.0;
    const bool nightmare = campaign && config.GetDifficultyPreset() == "Nightmare";
    // Publish small steps rather than a reliable packet for every tiny clock
    // change, while keeping the authoritative progression clock continuous.
    const auto challenge = ChallengeScaling::Evaluate(std::round(ModeChallengePressure() * 1000.0f) / 1000.0f);
    std::set<ObjectID> bosses;
    for (Object* object : graph->hotspots_) {
        if (!campaign) break;
        auto* hotspot = static_cast<Hotspot*>(object);
        if (hotspot->GetScriptFile().find("overgrowth_level") == std::string::npos || !hotspot->ASHasVar("progress")) continue;
        for (const auto& param : hotspot->GetScriptParams()->GetParameterMap()) {
            if (param.first.compare(0, 5, "goal_") != 0 || !param.second.IsString()) continue;
            std::istringstream tokens(param.second.GetString());
            std::string kind, token;
            tokens >> kind;
            if (kind != "defeat" && kind != "spawn_defeat" && kind != "defeat_optional") continue;
            const int goal = std::atoi(param.first.c_str() + 5);
            Encounter& encounter = state.encounters[{hotspot->GetID(), goal}];
            encounter.hotspot = hotspot->GetID(); encounter.goal = goal; encounter.originals.clear(); encounter.bosses.clear();
            int strongest = 0, top_count = 0;
            while (tokens >> token) {
                if (token.empty() || !std::isdigit(static_cast<unsigned char>(token[0]))) continue;
                const ObjectID id = std::atoi(token.c_str());
                MovementObject* actor = Actor(graph, id);
                if (!Hostile(this, graph, actor) || IsPlayerCharacter(id)) continue;
                encounter.originals.push_back(id);
                if (Strength(actor) > strongest) { strongest = Strength(actor); top_count = 1; }
                else if (Strength(actor) == strongest) ++top_count;
            }
            for (ObjectID id : encounter.originals) {
                MovementObject* actor = Actor(graph, id);
                const int rank = Strength(actor);
                const bool inferred = (encounter.originals.size() <= 2 && (rank >= CampaignDifficulty::CatOrDog || Elite(actor))) ||
                    (rank >= CampaignDifficulty::BigDog && rank == strongest && top_count == 1);
                if (ExplicitBoss(actor, inferred)) { encounter.bosses.insert(id); bosses.insert(id); }
            }
        }
    }
    int active_extras = 0;
    for (auto& entry : state.encounters) {
        Encounter& encounter = entry.second;
        Object* object = graph->GetObjectFromID(encounter.hotspot);
        const bool completed = object && object->GetType() == _hotspot_object &&
            static_cast<Hotspot*>(object)->ASGetIntVar("progress") > encounter.goal;
        encounter.extras.erase(std::remove_if(encounter.extras.begin(), encounter.extras.end(), [&](const Reinforcement& extra) {
            MovementObject* actor = Actor(graph, extra.actor);
            if (actor && Awake(actor)) {
                if (actor->enabled_) ++active_extras;
                return false;
            }
            if (!completed || (actor && NearestMainParty(this, graph, actor->position, true) < 65.0f)) return false;
            // Keep nearby ground loot visible too; never delete a taken item.
            for (ObjectID id : extra.equipment) {
                Object* item = graph->GetObjectFromID(id);
                if (item && item->GetType() == _item_object && static_cast<ItemObject*>(item)->HeldByWhom() == -1 &&
                    NearestMainParty(this, graph, item->GetTranslation(), true) < 65.0f) return false;
            }
            state.applied.erase(extra.actor);
            RemoveReinforcement(graph, extra);
            return true;
        }), encounter.extras.end());
    }
    auto apply = [&](MovementObject* actor, bool hostile, bool boss) {
        auto factors = hostile && !LobbyBot(this, actor->GetID()) ? CampaignDifficulty::ForParty(humans, bots, boss, Strength(actor), stats, nightmare,
                                                                     GetOriginalID(actor->GetID())) : CampaignDifficulty::Factors();
        if (hostile) {
            factors.vitality = std::min(6.0f, factors.vitality * challenge.vitality);
            factors.damage = std::min(2.0f, factors.damage * challenge.damage);
            factors.armor = std::min(2.0f, factors.armor * challenge.armor);
        }
        const std::array<float, 4> values = {{factors.vitality, factors.damage, factors.armor, factors.ai}};
        auto old = state.applied.find(actor->GetID());
        const char* names[] = {"campaign_vitality_mult", "campaign_damage_mult", "campaign_armor_mult", "campaign_ai_bonus"};
        bool installed = true;
        for (int i = 0; i < 4; ++i)
            installed = installed && actor->ASHasVar(names[i]) && std::fabs(actor->ASGetFloatVar(names[i]) - values[i]) < 0.00001f;
        // Character ResetGlobals can run after the level's reset message. A
        // cached send does not prove the host's new script has installed it.
        if (old != state.applied.end() && old->second == values && installed) return;
        if (!hostile && old == state.applied.end()) return;
        ReceiveCampaignScale(GetOriginalID(actor->GetID()), factors.vitality, factors.damage, factors.armor, factors.ai);
        Send<OnlineMessages::CampaignScale>(GetOriginalID(actor->GetID()), factors.vitality, factors.damage, factors.armor, factors.ai);
        state.applied[actor->GetID()] = values;
    };
    int active_hostiles = 0;
    for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        const bool hostile = Hostile(this, graph, actor);
        // Future scripted enemies elsewhere in a chapter are not part of the
        // current fight's budget. Generated enemies retain a separate global
        // concurrency cap, including any still fighting a distant party member.
        if (hostile && ReadyForReinforcements(actor) && Awake(actor) && NearestMainParty(this, graph, actor->position) < 55.0f) ++active_hostiles;
        if (actor->HasFunction("void SetCampaignScaling(float, float, float, float)"))
            apply(actor, hostile, bosses.count(actor->GetID()) != 0 || ExplicitBoss(actor, false));
    }
    if (!campaign) return;
    DeployOpponents(this, graph, state, now);
    if (!stats || humans + bots <= 1 || coop_cutscene || graph->level->DialogueCameraControl()) {
        RecheckReinforcementGoals(this, graph, state);
        return;
    }
    constexpr int kSpawnsPerUpdate = 2;
    int spawns = 0;
    for (auto& entry : state.encounters) {
        Encounter& encounter = entry.second;
        Object* object = graph->GetObjectFromID(encounter.hotspot);
        auto* hotspot = object && object->GetType() == _hotspot_object ? static_cast<Hotspot*>(object) : nullptr;
        if (!hotspot || hotspot->ASGetIntVar("progress") != encounter.goal) continue;
        std::vector<MovementObject*> sources;
        bool nearby = false;
        bool originals_awake = false;
        for (ObjectID id : encounter.originals) {
            MovementObject* actor = Actor(graph, id);
            if (!ReadyForReinforcements(actor)) continue;
            originals_awake = originals_awake || Awake(actor);
            if (!Awake(actor) && encounter.reinforcement_target <= encounter.spawned) continue;
            sources.push_back(actor);
            nearby = nearby || NearestMainParty(this, graph, actor->position) < 55.0f;
        }
        if (encounter.reinforcements_deferred) continue;
        if (sources.empty()) {
            // A removed/disabled story template cannot leave a pending wave
            // holding the objective forever.
            if (!originals_awake && encounter.reinforcement_target > encounter.spawned) encounter.reinforcements_deferred = true;
            continue;
        }
        if (!nearby) {
            if (!originals_awake && encounter.reinforcement_target > encounter.spawned) encounter.reinforcements_deferred = true;
            continue;
        }
        const bool boss_fight = !encounter.bosses.empty();
        encounter.reinforcement_target = EncounterReinforcements(graph, encounter, humans, bots);
        while (encounter.spawned < encounter.reinforcement_target && spawns < kSpawnsPerUpdate) {
            if (active_extras >= CampaignDifficulty::kConcurrentExtraCap || active_hostiles >= CampaignDifficulty::kActorCap) {
                // While the authored fight is ongoing, retry after casualties
                // free space. Once it is over, don't lock the story waiting
                // for an unrelated distant encounter to release its budget.
                if (!originals_awake) encounter.reinforcements_deferred = true;
                break;
            }
            MovementObject* source = nullptr;
            Reinforcement extra;
            std::vector<MovementObject*> candidates = sources;
            if (boss_fight) std::stable_sort(candidates.begin(), candidates.end(), [](MovementObject* a, MovementObject* b) { return Strength(a) < Strength(b); });
            else std::rotate(candidates.begin(), candidates.begin() + encounter.spawned % candidates.size(), candidates.end());
            for (MovementObject* candidate : candidates) {
                extra = Spawn(this, graph, candidate, encounter.spawned + 1);
                if (extra.actor >= 0) { source = candidate; break; }
            }
            if (extra.actor < 0) {
                encounter.reinforcements_deferred = true;
                LOGI << "Campaign scaling: no safe reinforcement ground for encounter " << encounter.hotspot << ":" << encounter.goal << ", deferred" << std::endl;
                break;
            }
            encounter.extras.push_back(extra); ++encounter.spawned; ++active_extras; ++active_hostiles; ++spawns;
            if (MovementObject* actor = Actor(graph, extra.actor)) apply(actor, true, false);
            LOGI << "Campaign scaling: reinforcement " << extra.actor << " from " << source->GetID() << ", humans " << humans << std::endl;
        }
    }
    RecheckReinforcementGoals(this, graph, state);
}
