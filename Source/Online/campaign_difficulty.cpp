// Party-size scaling for authored campaign encounters. Apache-2.0.
#include <Online/campaign_difficulty.h>
#include <Online/online.h>
#include <Online/online_utility.h>
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
#include <Logging/logdata.h>
#include <Scripting/angelscript/ascontext.h>
#include <Math/vec3math.h>
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
    int spawned = 0;  // dead reinforcements are never replaced
};
MovementObject* Actor(SceneGraph* graph, ObjectID id) {
    Object* object = graph ? graph->GetObjectFromID(id) : nullptr;
    return object && object->GetType() == _movement_object ? static_cast<MovementObject*>(object) : nullptr;
}
bool Awake(MovementObject* actor) {
    return actor && actor->ASHasVar("knocked_out") && actor->ASGetIntVar("knocked_out") == 0;
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
bool Hostile(Online* online, SceneGraph* graph, MovementObject* actor) {
    if (!actor || actor->controlled || online->IsPlayerCharacter(actor->GetID()) ||
        !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) return false;
    ScriptParams* params = actor->GetScriptParams();
    if (params->HasParam("Campaign Scaling Exempt") && params->ASGetInt("Campaign Scaling Exempt") != 0) return false;
    bool human = false;
    for (const auto& player : online->GetPlayerStates()) {
        if (player.is_bot) continue;
        MovementObject* avatar = Actor(graph, player.object_id);
        if (!avatar) continue;
        human = true;
        if (actor->ASOnSameTeam(avatar)) return false;
    }
    return human;
}
bool ExplicitBoss(MovementObject* actor, bool inferred) {
    ScriptParams* params = actor->GetScriptParams();
    return params->HasParam("Campaign Boss") ? params->ASGetInt("Campaign Boss") != 0 : inferred;
}
bool Elite(MovementObject* actor) {
    ScriptParams* params = actor->GetScriptParams();
    return (params->HasParam("Knockout Shield") && params->ASGetInt("Knockout Shield") >= 2) ||
           (params->HasParam("Damage Resistance") && params->ASGetFloat("Damage Resistance") >= 1.5f);
}
float NearestHuman(Online* online, SceneGraph* graph, const vec3& position) {
    float nearest = 1e30f;
    for (const auto& player : online->GetPlayerStates()) {
        if (player.is_bot) continue;
        if (MovementObject* actor = Actor(graph, player.object_id))
            if (Awake(actor)) nearest = std::min(nearest, distance(actor->position, position));
    }
    return nearest;
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
    bool found = false;
    for (int attempt = index + 1; attempt <= index + 12; ++attempt) {
        if (!online->FindSafeSpot(source->position, source->GetFacing(), attempt, &position, source->GetID())) continue;
        bool occupied = false;
        for (Object* object : graph->movement_objects_) {
            const vec3 offset = static_cast<MovementObject*>(object)->position - position;
            if (std::fabs(offset.y()) < 1.8f && offset.x() * offset.x() + offset.z() * offset.z() < 1.4f) { occupied = true; break; }
        }
        if (!occupied) { found = true; break; }
    }
    if (!found) return result;
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
    SceneGraph* graph = nullptr;
    std::map<std::pair<ObjectID, int>, Encounter> encounters;
    std::map<ObjectID, std::array<float, 4>> applied;
    bool reset = false;
    double next_update = 0;
    int spawned = 0;
};

int Online::CampaignHumanCount() const {
    int humans = 0;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (online_session) for (const auto& entry : online_session->player_states)
        if (!entry.second.is_bot && Actor(graph, entry.second.object_id)) ++humans;
    return std::max(1, humans);
}

void Online::ReceiveCampaignScale(CommonObjectID character, float vitality, float damage, float armor, float ai) {
    if (!std::isfinite(vitality) || !std::isfinite(damage) || !std::isfinite(armor) || !std::isfinite(ai) ||
        vitality < 1 || vitality > 6 || damage < 1 || damage > 2 || armor < 1 || armor > 2 || ai < 0 || ai > 0.5f) return;
    MovementObject* actor = Actor(Engine::Instance()->GetSceneGraph(), GetObjectID(character));
    if (!actor || !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) return;
    if (IsPlayerCharacter(actor->GetID()) && (vitality != 1 || damage != 1 || armor != 1 || ai != 0)) return;
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
    for (const auto& extra : it->second.extras) if (Awake(Actor(graph, extra.actor))) return false;
    return true;
}

void Online::UpdateCampaignDifficulty(SceneGraph* graph) {
    if (!IsHosting() || !host_started_level || loading || !graph || !graph->level || !CoopCampaign()) return;
    if (!campaign_difficulty_state || campaign_difficulty_state->graph != graph) {
        campaign_difficulty_state = std::make_shared<CampaignDifficultyState>();
        campaign_difficulty_state->graph = graph;
    }
    auto& state = *campaign_difficulty_state;
    const double now = OnlineClockMs();
    if (state.reset) {
        for (const auto& entry : state.encounters) for (const auto& extra : entry.second.extras) {
            for (ObjectID id : extra.equipment) {
                Object* object = graph->GetObjectFromID(id);
                if (object && object->GetType() == _item_object) {
                    const int holder = static_cast<ItemObject*>(object)->HeldByWhom();
                    if (holder == -1 || holder == extra.actor) graph->map_editor->DeleteID(id);
                }
            }
            if (Actor(graph, extra.actor)) graph->map_editor->DeleteID(extra.actor);
        }
        // A reset may also change allegiances. Restore every previous target
        // before rebuilding the roster, including a former enemy now allied.
        for (const auto& previous : state.applied) if (Actor(graph, previous.first)) {
            ReceiveCampaignScale(GetOriginalID(previous.first), 1, 1, 1, 0);
            Send<OnlineMessages::CampaignScale>(GetOriginalID(previous.first), 1, 1, 1, 0);
        }
        state.encounters.clear(); state.applied.clear(); state.spawned = 0; state.reset = false; state.next_update = 0;
    }
    if (now < state.next_update) return;
    state.next_update = now + 350.0;
    const int humans = CampaignHumanCount();
    const bool stats = config["campaign_mp_scaling"].toBool();
    const bool nightmare = config.GetDifficultyPreset() == "Nightmare";
    std::set<ObjectID> bosses;
    for (Object* object : graph->hotspots_) {
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
                if (!Hostile(this, graph, actor)) continue;
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
    auto apply = [&](MovementObject* actor, bool hostile, bool boss) {
        const auto factors = hostile ? CampaignDifficulty::ForPlayers(humans, boss, Strength(actor), stats, nightmare) : CampaignDifficulty::Factors();
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
        if (hostile && actor->enabled_ && Awake(actor)) ++active_hostiles;
        if (actor->HasFunction("void SetCampaignScaling(float, float, float, float)"))
            apply(actor, hostile, bosses.count(actor->GetID()) != 0 || ExplicitBoss(actor, false));
    }
    if (!stats || humans <= 1 || coop_cutscene || graph->level->DialogueCameraControl()) return;
    for (auto& entry : state.encounters) {
        Encounter& encounter = entry.second;
        Object* object = graph->GetObjectFromID(encounter.hotspot);
        auto* hotspot = object && object->GetType() == _hotspot_object ? static_cast<Hotspot*>(object) : nullptr;
        if (!hotspot || hotspot->ASGetIntVar("progress") != encounter.goal) continue;
        std::vector<MovementObject*> sources;
        int rank = 0;
        bool nearby = false;
        for (ObjectID id : encounter.originals) {
            MovementObject* actor = Actor(graph, id);
            if (!actor || !actor->enabled_ || !Awake(actor) || actor->static_char ||
                (actor->ASHasVar("dialogue_control") && actor->ASGetBoolVar("dialogue_control"))) continue;
            sources.push_back(actor); rank = std::max(rank, Strength(actor));
            nearby = nearby || NearestHuman(this, graph, actor->position) < 55.0f;
        }
        if (sources.empty() || !nearby) continue;
        const bool boss_fight = !encounter.bosses.empty();
        const int wanted = CampaignDifficulty::ExtraEnemies(humans, static_cast<int>(encounter.originals.size()), boss_fight, rank);
        while (encounter.spawned < wanted && state.spawned < CampaignDifficulty::kLevelExtraCap && active_hostiles < CampaignDifficulty::kActorCap) {
            MovementObject* source = sources[encounter.spawned % sources.size()];
            if (boss_fight) source = *std::min_element(sources.begin(), sources.end(), [](MovementObject* a, MovementObject* b) { return Strength(a) < Strength(b); });
            Reinforcement extra = Spawn(this, graph, source, encounter.spawned + 1);
            if (extra.actor < 0) break;
            encounter.extras.push_back(extra); ++encounter.spawned; ++state.spawned; ++active_hostiles;
            if (MovementObject* actor = Actor(graph, extra.actor)) apply(actor, true, false);
            LOGI << "Campaign scaling: reinforcement " << extra.actor << " from " << source->GetID() << ", humans " << humans << std::endl;
        }
    }
}
