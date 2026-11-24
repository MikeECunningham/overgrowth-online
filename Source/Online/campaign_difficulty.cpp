// Party-size scaling for authored campaign encounters. Apache-2.0.
#include <Online/campaign_difficulty.h>
#include <Online/challenge_scaling.h>
#include <Online/campaign_equipment.h>
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
#include <Objects/envobject.h>
#include <Internal/memwrite.h>
#include <Physics/bulletworld.h>
#include <Logging/logdata.h>
#include <JSON/json.h>
#include <Scripting/angelscript/ascontext.h>
#include <Math/vec3math.h>
#include <Graphics/camera.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <array>
#include <set>
#include <sstream>
#include <iomanip>

namespace {
struct Reinforcement {
    ObjectID actor = -1;
    std::vector<ObjectID> equipment;
    uint32_t seed = 0;
    int slot = 0;
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
        online->Send<OnlineMessages::CreateEntity>(source->obj_file, position, actor->GetID(), true);
        actor->ApplyNetworkCharacter(source->character_path);
        online->SendAvatarCharacter(actor->GetID(), source->character_path);
        const char* keys[] = {"Teams", "Aggression", "Damage Resistance", "Block Skill", "Block Follow-up", "Ground Aggression",
            "Attack Speed", "Attack Damage", "Attack Knockback", "Movement Speed", "Character Scale", "Knockout Shield",
            "Left handed", "Fat", "Muscle", "Ear Size", "Weapon Catch Skill", "Species",
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
        actor->GetScriptParams()->ASSetInt("Wearing Metal Armor", 0);
        actor->GetScriptParams()->ASAddIntCheckbox("No Save", true);
        actor->UpdateScriptParams();
        actor->ApplyPalette(*source->GetPalette());
        // Generated units begin unarmed. Their separate equipment plan, not
        // the template's late-story inventory, decides if/when they get arms.
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
    struct Equipment {
        CampaignEquipment::Plan plan;
        std::vector<ObjectID> armor;
        std::vector<ObjectID> weapons;
        int applied_regions = 0;
        int weapons_given = 0;
        bool weapon_done = false;
    };
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
    std::map<ObjectID, Equipment> gear;
    ObjectID gear_cursor = -1;
    uint32_t equipment_epoch = 0;
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
struct ArmorTemplate {
    int region;
    EntityDescription description;
    AttachedEnvObject attachment = {};
    std::string path;
    vec3 scale = vec3(1.0f);
};
const std::vector<ArmorTemplate>& ArmorTemplates() {
    static const std::vector<ArmorTemplate> templates = [] {
        std::vector<ArmorTemplate> result;
        EntityDescriptionList descriptions;
        std::string type; Path file;
        ActorsEditor_LoadEntitiesFromFile("Data/Objects/lugaru_campaign/turner_armored.xml", descriptions, &type, &file);
        for (const auto& actor : descriptions) {
            if (GetTypeFromDesc(actor) != _movement_object) continue;
            const auto* field = actor.GetField(EDF_ENV_OBJECT_ATTACH);
            if (!field) continue;
            std::vector<AttachedEnvObject> attachments;
            Deserialize(field->data, attachments);
            const int indices[] = {0, 1, 2, 3, 5, 7, 10, 11};
            const int regions[] = {1, 2, 8, 16, 32, 64, 128, 256};
            for (int n = 0; n < 8; ++n) {
                const unsigned i = indices[n];
                if (i >= actor.children.size() || i >= attachments.size() || GetTypeFromDesc(actor.children[i]) != _env_object) continue;
                ArmorTemplate entry;
                entry.region = regions[n]; entry.description = actor.children[i]; entry.attachment = attachments[i]; entry.attachment.direct_ptr = nullptr;
                if (const auto* path_field = entry.description.GetField(EDF_FILE_PATH)) { Path path; path_field->ReadPath(&path); entry.path = path.GetOriginalPathStr(); }
                if (const auto* scale = entry.description.GetField(EDF_SCALE)) memread(entry.scale.entries, sizeof(float), 3, scale->data);
                if (!entry.path.empty()) result.push_back(entry);
            }
        }
        // Stock helmet's calibrated head-bone fit (the same biped rig family).
        descriptions.clear();
        ActorsEditor_LoadEntitiesFromFile("Data/Objects/Gear/dog_armor/dog_armor_helmet.xml", descriptions, &type, &file);
        if (!descriptions.empty()) {
            ArmorTemplate helmet;
            helmet.region = CampaignEquipment::Head; helmet.description = descriptions.front();
            helmet.path = "Data/Objects/Gear/dog_armor/dog_armor_helmet.xml";
            helmet.scale = vec3(1.26937f);
            for (auto& bone : helmet.attachment.bone_connects) { bone.bone_id = -1; bone.num_connections = 0; bone.rel_mat = mat4(1.0f); }
            auto& head = helmet.attachment.bone_connects[0]; head.bone_id = 31; head.num_connections = 1;
            const float fit[] = {0.99596f,-0.0389717f,-0.0809086f,0,0.057344f,0.969329f,0.238984f,0,0.0691134f,-0.242658f,0.967647f,0,0.00782776f,0.626611f,0.348953f,1};
            for (int i = 0; i < 16; ++i) head.rel_mat[i] = fit[i];
            result.push_back(helmet);
        }
        return result;
    }();
    return templates;
}
bool BoneOnChain(MovementObject* actor, const char* chain, int target) {
    const auto& skeleton = actor->rigged_object()->skeleton_;
    const auto found = skeleton.simple_ik_bones.find(chain);
    if (found == skeleton.simple_ik_bones.end()) return false;
    int bone = found->second.bone_id;
    for (int n = 0; n < 5 && bone >= 0 && unsigned(bone) < skeleton.parents.size(); ++n) {
        if (bone == target) return true;
        bone = skeleton.parents[bone];
    }
    return false;
}
int AuthoredPieceRegions(MovementObject* actor, Object* object, const AttachedEnvObject& attachment, int depth = 0) {
    if (!object || depth > 5 || object->GetScriptParams()->HasParam("Campaign Armor Region")) return 0;
    std::string path = object->obj_file;
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return std::tolower(c); });
    int region = 0;
    if (path.find("breastplate") != std::string::npos || path.find("chestplate") != std::string::npos) region = CampaignEquipment::Front;
    else if (path.find("backplate") != std::string::npos) region = CampaignEquipment::Back;
    else if (path.find("helmet") != std::string::npos) region = CampaignEquipment::Head;
    else {
        const auto* bone = &attachment.bone_connects[0];
        for (const auto& candidate : attachment.bone_connects) if (candidate.num_connections > bone->num_connections) bone = &candidate;
        const bool left_arm = BoneOnChain(actor, "leftarm", bone->bone_id), right_arm = BoneOnChain(actor, "rightarm", bone->bone_id);
        const bool left_leg = BoneOnChain(actor, "left_leg", bone->bone_id), right_leg = BoneOnChain(actor, "right_leg", bone->bone_id);
        const bool left = left_arm != right_arm ? left_arm : left_leg != right_leg ? left_leg : bone->rel_mat.GetTranslationPart().x() >= 0;
        if (path.find("lower_arm") != std::string::npos || path.find("armchunk") != std::string::npos) region = left ? CampaignEquipment::LeftForearm : CampaignEquipment::RightForearm;
        else if (path.find("shoulder") != std::string::npos && (path.find("armor") != std::string::npos || path.find("armour") != std::string::npos)) region = left ? CampaignEquipment::LeftShoulder : CampaignEquipment::RightShoulder;
        else if (path.find("hip_plate") != std::string::npos || path.find("thighpiece") != std::string::npos) region = left ? CampaignEquipment::LeftThigh : CampaignEquipment::RightThigh;
    }
    if (object->IsGroupDerived()) {
        std::vector<Object*> children;
        object->GetChildren(&children);
        for (Object* child : children) region |= AuthoredPieceRegions(actor, child, attachment, depth + 1);
    }
    return region;
}
void PublishArmor(Online* online, MovementObject* actor, int mask) {
    const char* keys[] = {"campaign_vitality_mult", "campaign_damage_mult", "campaign_armor_mult", "campaign_ai_bonus"};
    float values[] = {1, 1, 1, 0};
    for (int i = 0; i < 4; ++i) if (actor->ASHasVar(keys[i])) values[i] = actor->ASGetFloatVar(keys[i]);
    if (online->IsPlayerCharacter(actor->GetID()) && !LobbyBot(online, actor->GetID())) { values[0] = values[1] = values[2] = 1; values[3] = 0; }
    if (actor->ASHasVar("campaign_armor_regions")) actor->ASSetIntVar("campaign_armor_regions", mask);
    online->ReceiveCampaignScale(online->GetOriginalID(actor->GetID()), values[0], values[1], 1, values[3], mask);
    online->Send<OnlineMessages::CampaignScale>(online->GetOriginalID(actor->GetID()), values[0], values[1], 1, values[3], mask);
}
ObjectID AddArmorPiece(Online* online, SceneGraph* graph, MovementObject* actor, const ArmorTemplate& piece) {
    auto* rigged = actor->rigged_object();
    if (rigged->display_bone_transforms.empty()) return -1;
    AttachedEnvObject attachment = piece.attachment;
    int count = 0;
    const float scale = rigged->GetCharScale();
    vec3 piece_scale = piece.scale;
    if (!rigged->skeleton_.skeleton_asset_ref.valid()) return -1;
    const auto& target_data = rigged->skeleton_.skeleton_asset_ref->GetData();
    const float model_scale = scale / actor->character_script_getter.GetModelScale();
    if (!std::isfinite(model_scale) || model_scale <= 0) return -1;
    SkeletonAssetRef turner;
    if (piece.region != CampaignEquipment::Head) turner = Engine::Instance()->GetAssetManager()->LoadSync<SkeletonAsset>("Data/Skeletons/r_turner.xml");
    if (piece.region == CampaignEquipment::Head) {
        const auto head = rigged->skeleton_.simple_ik_bones.find("head");
        if (head == rigged->skeleton_.simple_ik_bones.end() || head->second.bone_id < 0 ||
            unsigned(head->second.bone_id) >= target_data.bone_mats.size()) return -1;
        auto& fit = attachment.bone_connects[0];
        fit.bone_id = head->second.bone_id;
        const bool rabbit = actor->ASHasVar("species") && (actor->ASGetIntVar("species") == 0 || actor->ASGetIntVar("species") == 3);
        // Genuine stock-mesh fits on the rabbit and dog rig families. Only
        // numeric calibration is reused; the mesh always comes from base Data.
        SkeletonAssetRef reference_rig = Engine::Instance()->GetAssetManager()->LoadSync<SkeletonAsset>(rabbit ? "Data/Skeletons/r_rig.xml" : "Data/Skeletons/r_lt_dog.xml");
        const auto& reference_data = reference_rig->GetData();
        const auto reference_head = reference_data.simple_ik_bones.find("head");
        if (reference_head == reference_data.simple_ik_bones.end() || reference_head->second.bone_id < 0 ||
            unsigned(reference_head->second.bone_id) >= reference_data.bone_mats.size()) return -1;
        const vec3 reference_bind = reference_data.bone_mats[reference_head->second.bone_id].GetTranslationPart();
        // Guard's FZX head sphere has a COM distinct from the bone midpoint.
        const vec3 reference_com = rabbit ? vec3(-0.0004724315f,0.5055459594f,0.0533621245f) : vec3(-0.0004800771f,0.6370600582f,0.2531268824f);
        const float reference_scale = rabbit ? 1.12f : 1.3f;
        if (rabbit) {
            const float rabbit_fit[] = {0.994659f,0.0105678f,0.102674f,0,0.0129891f,0.974021f,-0.226085f,0,-0.102396f,0.226211f,0.968681f,0,-0.0171747f,0.531875f,0.158254f,1};
            for (int i = 0; i < 16; ++i) fit.rel_mat[i] = rabbit_fit[i];
        }
        piece_scale = vec3((rabbit ? 1.09475f : 1.26937f) / reference_scale);
        const vec3 target_bind = target_data.bone_mats[fit.bone_id].GetTranslationPart();
        const vec3 target_com = rigged->skeleton_.physics_bones[fit.bone_id].initial_position / model_scale;
        const vec3 origin = fit.rel_mat.GetTranslationPart();
        vec3 position;
        for (int i = 0; i < 3; ++i) position[i] = CampaignEquipment::RetargetAnchor(origin[i], reference_bind[i], reference_com[i], reference_scale, target_bind[i], target_com[i], scale, model_scale);
        fit.rel_mat.SetTranslationPart(position);
    }
    for (auto& bone : attachment.bone_connects) if (bone.num_connections) {
        if (bone.bone_id < 0 || unsigned(bone.bone_id) >= rigged->display_bone_transforms.size()) return -1;
        if (piece.region != CampaignEquipment::Head) {
            const auto& reference = turner->GetData();
            if (unsigned(bone.bone_id) >= reference.bone_mats.size() || unsigned(bone.bone_id) >= target_data.bone_mats.size()) return -1;
            const vec3 reference_bind = reference.bone_mats[bone.bone_id].GetTranslationPart();
            const vec3 target_bind = target_data.bone_mats[bone.bone_id].GetTranslationPart();
            const vec3 target_com = rigged->skeleton_.physics_bones[bone.bone_id].initial_position / model_scale;
            const vec3 original = bone.rel_mat.GetTranslationPart();
            vec3 position;
            for (int i = 0; i < 3; ++i) position[i] = CampaignEquipment::RetargetAnchor(original[i], reference_bind[i], reference_bind[i], 1.0f, target_bind[i], target_com[i], scale, model_scale);
            bone.rel_mat.SetTranslationPart(position);
        }
        count += bone.num_connections;
    }
    if (!count) return -1;
    mat4 world(0.0f);
    for (const auto& bone : attachment.bone_connects) if (bone.num_connections)
        world += rigged->display_bone_transforms[bone.bone_id].GetMat4() * bone.rel_mat * (float(bone.num_connections) / count);
    for (int i = 0; i < 16; ++i) if (!std::isfinite(world[i])) return -1;
    EntityDescription desc = piece.description;
    ClearDescID(&desc); desc.children.clear();
    desc.fields.erase(std::remove_if(desc.fields.begin(), desc.fields.end(), [](const EntityDescriptionField& field) {
        return field.type == EDF_TRANSLATION || field.type == EDF_ROTATION || field.type == EDF_SCALE || field.type == EDF_NO_NAVMESH;
    }), desc.fields.end());
    desc.AddVec3(EDF_TRANSLATION, world.GetTranslationPart());
    desc.AddQuaternion(EDF_ROTATION, QuaternionFromMat4(world.GetRotationPart()));
    desc.AddVec3(EDF_SCALE, piece_scale * scale); desc.AddBool(EDF_NO_NAVMESH, true);
    Object* object = MapEditor::AddEntityFromDesc(graph, desc, false);
    if (!object || object->GetType() != _env_object) return -1;
    object->exclude_from_save = true; object->exclude_from_undo = true; object->created_on_the_fly = true;
    object->SetCollisionEnabled(false); object->SetParent(actor);
    ScriptParam provenance; provenance.SetInt(piece.region); object->GetScriptParams()->InsertScriptParam("Campaign Armor Region", provenance);
    attachment.direct_ptr = object; attachment.legacy_obj_id = object->GetID(); attachment.bone_connection_dirty = false;
    rigged->children.push_back(attachment);
    online->Send<OnlineMessages::CreateEntity>(piece.path, world.GetTranslationPart(), object->GetID());
    online->Send<OnlineMessages::AttachToMessage>(actor->GetID(), object->GetID(), 0, true, false, &attachment, piece_scale * scale, piece.region);
    return object->GetID();
}
void UpdateEquipment(Online* online, SceneGraph* graph, CampaignDifficultyState& state, int humans, int bots, bool party_stats) {
    const bool held = online->CoopCutscene() || graph->level->DialogueCameraControl();
    const bool master = online->DifficultyScalingEnabled();
    const bool armor = master && config["campaign_armor_scaling"].toBool();
    const bool weapons = master && config["campaign_weapon_scaling"].toBool();
    for (auto it = state.gear.begin(); it != state.gear.end();) {
        MovementObject* actor = Actor(graph, it->first);
        if (!actor || !Hostile(online, graph, actor)) {
            for (ObjectID id : it->second.armor) if (graph->GetObjectFromID(id)) graph->map_editor->DeleteID(id);
            if (actor) PublishArmor(online, actor, 0);
            it = state.gear.erase(it);
        } else ++it;
    }
    const double intensity = CampaignEquipment::Intensity(online->ModeChallengePressure(), party_stats ? CampaignDifficulty::ExtraPlayersForParty(humans, bots) : 0);
    for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        bool inherited = actor->GetScriptParams()->HasParam("Campaign Weapon Issued");
        for (const auto& child : actor->rigged_object()->children) if (child.direct_ptr && child.direct_ptr->GetScriptParams()->HasParam("Campaign Armor Region")) inherited = true;
        if (!Hostile(online, graph, actor) || state.gear.count(actor->GetID()) || (!inherited &&
            (held || (!armor && !weapons) || !ReadyForReinforcements(actor) || !Awake(actor) || !actor->ASHasVar("time") ||
             actor->ASGetFloatVar("time") <= 0 || NearestMainParty(online, graph, actor->position) > 55))) continue;
        Reinforcement* extra = nullptr; Encounter* encounter = nullptr;
        for (auto& entry : state.encounters) for (auto& candidate : entry.second.extras) if (candidate.actor == actor->GetID()) { extra = &candidate; encounter = &entry.second; }
        auto& gear = state.gear[actor->GetID()];
        gear.plan = CampaignEquipment::ForUnit(extra ? extra->seed : uint32_t(online->GetOriginalID(actor->GetID())), intensity, actor->ASHasVar("campaign_armor_regions"), false, Strength(actor) == CampaignDifficulty::Wolf);
        const int authored = online->AuthoredArmorRegions(actor->GetID());
        gear.plan.armor_regions &= ~authored;
        if (actor->ASHasVar("g_wearing_metal_armor") && actor->ASGetBoolVar("g_wearing_metal_armor") && (!config["armor_localized"].toBool() || !authored)) gear.plan.armor_regions = 0;
        if (extra && encounter && !encounter->originals.empty()) {
            const uint32_t base = CampaignEquipment::ExtraSeed(online->GetOriginalID(encounter->originals.front()), online->GetOriginalID(encounter->hotspot), encounter->goal, 0);
            gear.plan.weapon = CampaignEquipment::WaveWeapon(base, extra->slot, std::max(encounter->spawned, encounter->reinforcement_target), intensity, Strength(actor) == CampaignDifficulty::Wolf);
        }
        gear.weapon_done = gear.plan.weapon == CampaignEquipment::Unarmed;
        // A promoted host already has these replicated pieces. Adopt them
        // rather than treating them as authored armor or manufacturing copies.
        for (const auto& child : actor->rigged_object()->children) if (child.direct_ptr && child.direct_ptr->GetScriptParams()->HasParam("Campaign Armor Region")) {
            const int region = child.direct_ptr->GetScriptParams()->ASGetInt("Campaign Armor Region") & CampaignEquipment::kAllRegions;
            gear.armor.push_back(child.direct_ptr->GetID()); gear.applied_regions |= region; gear.plan.armor_regions |= region;
            child.direct_ptr->exclude_from_save = true; child.direct_ptr->exclude_from_undo = true;
        }
        if (gear.applied_regions && actor->ASHasVar("campaign_armor_regions") && actor->ASGetIntVar("campaign_armor_regions") != gear.applied_regions) PublishArmor(online, actor, gear.applied_regions);
        if (extra) {
            for (ObjectID id : extra->equipment) {
                Object* item = graph->GetObjectFromID(id);
                if (item && item->GetType() == _item_object) {
                    gear.weapons.push_back(id); ++gear.weapons_given; gear.weapon_done = true;
                    const auto& label = static_cast<ItemObject*>(item)->item_ref()->GetLabel();
                    if (gear.plan.weapon == CampaignEquipment::Unarmed) gear.plan.weapon = label == "spear" ? CampaignEquipment::Spear : label == "knife" ? CampaignEquipment::Dagger : CampaignEquipment::Sword;
                }
            }
            if (actor->GetScriptParams()->HasParam("Campaign Weapon Issued") && actor->GetScriptParams()->ASGetInt("Campaign Weapon Issued")) gear.weapon_done = true;
        }
    }
    for (auto& entry : state.gear) if (MovementObject* actor = Actor(graph, entry.first)) {
        auto& gear = entry.second;
        if (!armor && !gear.armor.empty()) {
            for (ObjectID id : gear.armor) if (graph->GetObjectFromID(id)) graph->map_editor->DeleteID(id);
            gear.armor.clear(); gear.applied_regions = 0; PublishArmor(online, actor, 0);
        }
        if (!weapons) for (auto it = gear.weapons.begin(); it != gear.weapons.end();) {
            Object* object = graph->GetObjectFromID(*it);
            if (object && object->GetType() == _item_object && static_cast<ItemObject*>(object)->HeldByWhom() == actor->GetID()) {
                graph->map_editor->DeleteID(*it); gear.weapons_given = std::max(0, gear.weapons_given - 1); gear.weapon_done = false;
                if (!gear.weapons_given) actor->GetScriptParams()->ASSetInt("Campaign Weapon Issued", 0);
                it = gear.weapons.erase(it);
            } else ++it;  // dropped or taken loot is never deleted or replaced
        }
    }
    if (held || (!armor && !weapons)) return;
    int budget = 2;  // shared creation budget for gear, not per character
    for (auto& entry : state.gear) {
        auto& gear = entry.second;
        MovementObject* actor = Actor(graph, entry.first);
        if (!weapons || !budget || gear.weapon_done || gear.plan.weapon == CampaignEquipment::Unarmed || !actor || !Awake(actor) || !actor->enabled_) continue;
        if (!gear.weapons_given) {
            for (int i = 0; i < 4; ++i) if (actor->ASGetArrayIntVar("weapon_slots", i) != -1) gear.weapon_done = true;
            if (gear.weapon_done) continue;  // it picked up a weapon: don't replace loot
        }
        const bool knife = gear.plan.weapon == CampaignEquipment::Dagger;
        const std::string path = knife ? "Data/Items/rabbit_weapons/rabbit_knife.xml" : gear.plan.weapon == CampaignEquipment::Sword ? "Data/Items/DogWeapons/DogSword.xml" : "Data/Items/DogWeapons/DogSpear.xml";
        const int id = graph->map_editor->CreateObject(path);
        Object* object = graph->GetObjectFromID(id);
        if (!object || object->GetType() != _item_object) { gear.weapon_done = true; continue; }
        --budget; object->SetTranslation(actor->position); object->exclude_from_save = true; object->exclude_from_undo = true;
        online->Send<OnlineMessages::CreateEntity>(path, actor->position, id);
        object->GetScriptParams()->ASSetInt("Campaign Weapon Owner", online->GetOriginalID(actor->GetID()));
        object->GetScriptParams()->ASSetInt("Campaign Weapon Epoch", actor->GetScriptParams()->ASGetInt("Campaign Scaling Epoch"));
        // Runtime pickup, not an editor starting-loadout connection: retries
        // must never steal this weapon back from a human who took it.
        actor->Execute("AttachWeapon(" + std::to_string(id) + ");");
        const bool attached = static_cast<ItemObject*>(object)->HeldByWhom() == actor->GetID();
        if (!attached) { graph->map_editor->DeleteID(id); gear.weapon_done = true; continue; }
        const bool mirror = actor->ASGetIntVar("primary_weapon_slot") == 0;
        online->Send<OnlineMessages::AttachToMessage>(actor->GetID(), id, _at_grip, true, mirror, nullptr, vec3(1.0f), 0, true);
        for (auto& encounter : state.encounters) for (auto& extra : encounter.second.extras) if (extra.actor == actor->GetID()) extra.equipment.push_back(id);
        ++gear.weapons_given;
        gear.weapons.push_back(id);
        gear.weapon_done = gear.weapons_given >= 1;
        actor->GetScriptParams()->ASSetInt("Campaign Weapon Issued", 1);
    }
    auto begin = state.gear.upper_bound(state.gear_cursor);
    for (size_t n = 0; armor && budget && n < state.gear.size(); ++n) {
        if (begin == state.gear.end()) begin = state.gear.begin();
        const ObjectID id = begin->first; auto& gear = begin->second; ++begin;
        MovementObject* actor = Actor(graph, id);
        if (!actor || !actor->enabled_ || !Awake(actor)) continue;
        for (const auto& piece : ArmorTemplates()) if ((gear.plan.armor_regions & piece.region) && !(gear.applied_regions & piece.region)) {
            bool duplicate = false;
            for (const auto& child : actor->rigged_object()->children) if (child.direct_ptr && child.direct_ptr->obj_file == piece.path &&
                child.bone_connects[0].bone_id == piece.attachment.bone_connects[0].bone_id) duplicate = true;
            if (duplicate) { gear.plan.armor_regions &= ~piece.region; continue; }
            const ObjectID added = AddArmorPiece(online, graph, actor, piece);
            if (added < 0) continue;
            --budget; gear.armor.push_back(added); gear.applied_regions |= piece.region; state.gear_cursor = id;
            PublishArmor(online, actor, gear.applied_regions);
            break;
        }
    }
}
void AdoptReinforcements(Online* online, SceneGraph* graph, CampaignDifficultyState& state) {
    std::set<ObjectID> tracked;
    for (const auto& entry : state.encounters) for (const auto& extra : entry.second.extras) tracked.insert(extra.actor);
    for (Object* object : graph->objects_) if (object->GetType() == _item_object && object->GetScriptParams()->HasParam("Campaign Weapon Epoch"))
        state.equipment_epoch = std::max(state.equipment_epoch, uint32_t(object->GetScriptParams()->ASGetInt("Campaign Weapon Epoch")));
    for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        ScriptParams* params = actor->GetScriptParams();
        if (online->IsPlayerCharacter(actor->GetID()) || tracked.count(actor->GetID()) || !params->HasParam("Campaign Scaling Epoch") ||
            !params->HasParam("Campaign Scaling Hotspot") || !params->HasParam("Campaign Scaling Slot")) continue;
        const ObjectID hotspot = online->GetObjectID(params->ASGetInt("Campaign Scaling Hotspot"));
        const int goal = params->ASGetInt("Campaign Scaling Goal"), slot = params->ASGetInt("Campaign Scaling Slot");
        auto found = state.encounters.find({hotspot, goal});
        if (found == state.encounters.end() || slot < 0 || slot >= CampaignDifficulty::kRegularExtraCap) continue;
        Reinforcement extra;
        extra.actor = actor->GetID(); extra.slot = slot; extra.seed = uint32_t(params->ASGetInt("Campaign Scaling Seed"));
        const int epoch = params->ASGetInt("Campaign Scaling Epoch");
        state.equipment_epoch = std::max(state.equipment_epoch, uint32_t(epoch));
        for (Object* item : graph->objects_) if (item->GetType() == _item_object && item->GetScriptParams()->HasParam("Campaign Weapon Owner") &&
            online->GetObjectID(item->GetScriptParams()->ASGetInt("Campaign Weapon Owner")) == actor->GetID() &&
            item->GetScriptParams()->HasParam("Campaign Weapon Epoch") && item->GetScriptParams()->ASGetInt("Campaign Weapon Epoch") == epoch) extra.equipment.push_back(item->GetID());
        actor->created_on_the_fly = true; actor->exclude_from_save = true; actor->exclude_from_undo = true;
        found->second.extras.push_back(extra); found->second.spawned = std::max(found->second.spawned, slot + 1);
        found->second.reinforcement_target = std::max(found->second.reinforcement_target, found->second.spawned);
    }
}
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

void Online::ReceiveCampaignScale(CommonObjectID character, float vitality, float damage, float armor, float ai, int armor_regions) {
    if (!std::isfinite(vitality) || !std::isfinite(damage) || !std::isfinite(armor) || !std::isfinite(ai) ||
        vitality < 0.75f || vitality > 6 || damage < 0.9625f || damage > 2 || armor < 0.975f || armor > 2 || ai < 0 || ai > 0.03f ||
        armor_regions < -1 || armor_regions > CampaignEquipment::kAllRegions) return;
    MovementObject* actor = Actor(Engine::Instance()->GetSceneGraph(), GetObjectID(character));
    if (!actor || !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) return;
    if (IsPlayerCharacter(actor->GetID()) && !LobbyBot(this, actor->GetID()) && (vitality != 1 || damage != 1 || armor != 1 || ai != 0 || armor_regions > 0)) return;
    ASArglist args;
    args.Add(vitality); args.Add(damage); args.Add(armor); args.Add(ai);
    actor->as_context->CallScriptFunction("void SetCampaignScaling(float, float, float, float)", &args);
    if (armor_regions >= 0 && actor->ASHasVar("campaign_armor_regions")) actor->ASSetIntVar("campaign_armor_regions", armor_regions);
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
    if (DifficultyScalingEnabled() && config["campaign_mp_scaling"].toBool() && !it->second.reinforcements_deferred &&
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
    const bool master = DifficultyScalingEnabled();
    const bool ai_scaling = master && config["campaign_ai_scaling"].toBool();
    const bool equipment_scaling = master && (config["campaign_armor_scaling"].toBool() || config["campaign_weapon_scaling"].toBool());
    if (!campaign && !campaign_difficulty_state && ModeChallengePressure() == 0 && !ai_scaling && !equipment_scaling) {
        bool inherited = false;
        for (Object* object : graph->movement_objects_) {
            auto* actor = static_cast<MovementObject*>(object);
            inherited = inherited || actor->GetScriptParams()->HasParam("Campaign Scaling Epoch");
            for (const auto& child : actor->rigged_object()->children) if (child.direct_ptr && child.direct_ptr->GetScriptParams()->HasParam("Campaign Armor Region")) inherited = true;
        }
        if (!inherited) return;
    }
    if (!campaign_difficulty_state || campaign_difficulty_state->graph != graph) {
        campaign_difficulty_state = std::make_shared<CampaignDifficultyState>();
        campaign_difficulty_state->graph = graph;
    }
    auto& state = *campaign_difficulty_state;
    const double now = OnlineClockMs();
    if (state.reset) {
        for (const auto& entry : state.gear) {
            for (ObjectID id : entry.second.armor) if (graph->GetObjectFromID(id)) graph->map_editor->DeleteID(id);
            if (MovementObject* actor = Actor(graph, entry.first)) PublishArmor(this, actor, 0);
        }
        state.gear.clear(); state.gear_cursor = -1;
        for (const auto& entry : state.encounters) for (const auto& extra : entry.second.extras) RemoveReinforcement(graph, extra);
        // A reset may also change allegiances. Restore every previous target
        // before rebuilding the roster, including a former enemy now allied.
        for (const auto& previous : state.applied) if (Actor(graph, previous.first)) {
            ReceiveCampaignScale(GetOriginalID(previous.first), 1, 1, 1, 0, 0);
            Send<OnlineMessages::CampaignScale>(GetOriginalID(previous.first), 1, 1, 1, 0, 0);
        }
        state.encounters.clear(); state.applied.clear(); state.reset = false; state.next_update = 0;
        state.opponents.clear(); state.engaged.clear(); state.deployment = 0; state.combat = false; state.last_combat = -100000;
        state.deployment_target = -1; state.deployment_seats.clear();
        state.member_combat.clear(); state.active_encounter = {-1, -1}; state.last_deployment = -100000;
    }
    if (campaign) StageOpponents(this, graph, state);
    const int humans = CampaignHumanCount();
    const int bots = MainPartyBots(this, graph);
    const bool stats = campaign && master && config["campaign_mp_scaling"].toBool();
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
    AdoptReinforcements(this, graph, state);
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
        auto factors = hostile && !LobbyBot(this, actor->GetID()) ? CampaignDifficulty::ForParty(humans, bots, boss, Strength(actor), stats, ai_scaling,
                                                                     GetOriginalID(actor->GetID())) : CampaignDifficulty::Factors();
        if (hostile) {
            factors.vitality = std::min(6.0f, factors.vitality * challenge.vitality);
            factors.damage = std::min(2.0f, factors.damage * challenge.damage);
            factors.armor = 1;
            if (ai_scaling && !LobbyBot(this, actor->GetID()) && CampaignDifficulty::TweakForUnit(GetOriginalID(actor->GetID())) != CampaignDifficulty::NoTweak)
                factors.ai = CampaignDifficulty::AIAmount(humans, ModeChallengePressure());
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
        // Persistent scale messages coalesce by actor. Keep the current mask
        // with newer stats so a late join never loses its armor protection.
        const int armor_regions = actor->ASHasVar("campaign_armor_regions") ? actor->ASGetIntVar("campaign_armor_regions") : 0;
        Send<OnlineMessages::CampaignScale>(GetOriginalID(actor->GetID()), factors.vitality, factors.damage, factors.armor, factors.ai, armor_regions);
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
    UpdateEquipment(this, graph, state, humans, bots, stats);
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
            extra.slot = encounter.spawned;
            extra.seed = CampaignEquipment::ExtraSeed(GetOriginalID(encounter.originals.front()), GetOriginalID(encounter.hotspot), encounter.goal, extra.slot);
            if (MovementObject* actor = Actor(graph, extra.actor)) {
                actor->GetScriptParams()->ASSetInt("Campaign Scaling Hotspot", GetOriginalID(encounter.hotspot));
                actor->GetScriptParams()->ASSetInt("Campaign Scaling Goal", encounter.goal);
                actor->GetScriptParams()->ASSetInt("Campaign Scaling Slot", extra.slot);
                actor->GetScriptParams()->ASSetInt("Campaign Scaling Seed", static_cast<int>(extra.seed));
                actor->GetScriptParams()->ASSetInt("Campaign Scaling Epoch", static_cast<int>(++state.equipment_epoch));
            }
            encounter.extras.push_back(extra); ++encounter.spawned; ++active_extras; ++active_hostiles; ++spawns;
            if (MovementObject* actor = Actor(graph, extra.actor)) apply(actor, true, false);
            LOGI << "Campaign scaling: reinforcement " << extra.actor << " from " << source->GetID() << ", humans " << humans << std::endl;
        }
    }
    RecheckReinforcementGoals(this, graph, state);
}

struct DifficultyHUDState {
    SceneGraph* graph = nullptr;
    std::string level, payload;
    double next_update = 0;
};

namespace {
struct HUDContext {
    Online* online;
    SceneGraph* graph;
    int humans = 1, bots = 0;
    bool campaign = false, stats = false, ai_enabled = false;
    double pressure = 0, next_pressure = 0;
};
using HUDFactors = std::array<float, 4>;
HUDFactors InstalledHUDFactors(MovementObject* actor) {
    HUDFactors values = {{1, 1, 1, 0}};
    const char* names[] = {"campaign_vitality_mult", "campaign_damage_mult", "campaign_armor_mult", "campaign_ai_bonus"};
    for (int i = 0; actor && i < 4; ++i) if (actor->ASHasVar(names[i])) {
        const float value = actor->ASGetFloatVar(names[i]);
        if (std::isfinite(value) && value >= 0) values[i] = value;
    }
    return values;
}
HUDFactors PredictedHUDFactors(const HUDContext& ctx, MovementObject* source, bool boss, bool generated) {
    const bool bot = source && LobbyBot(ctx.online, source->GetID());
    auto values = ctx.campaign && !bot ? CampaignDifficulty::ForParty(ctx.humans, ctx.bots, boss,
        source ? Strength(source) : CampaignDifficulty::Rabbit, ctx.stats, ctx.ai_enabled,
        source && !generated ? ctx.online->GetOriginalID(source->GetID()) : CampaignDifficulty::kUnspecifiedUnit) : CampaignDifficulty::Factors();
    // Match the live publisher's millipressure quantization and caps.
    const auto growth = ChallengeScaling::Evaluate(std::round(static_cast<float>(ctx.next_pressure) * 1000.0) / 1000.0);
    if (ctx.ai_enabled && !bot && source && !generated && CampaignDifficulty::TweakForUnit(ctx.online->GetOriginalID(source->GetID())) != CampaignDifficulty::NoTweak)
        values.ai = CampaignDifficulty::AIAmount(ctx.humans, ctx.next_pressure);
    return {{std::min(6.0f, values.vitality * growth.vitality), std::min(2.0f, values.damage * growth.damage),
             1, values.ai}};
}
const char* HUDWeaponLabel(int tier) { return tier == CampaignEquipment::Spear ? "spear" : tier == CampaignEquipment::Sword ? "sword" : tier == CampaignEquipment::Dagger ? "dagger" : "unarmed"; }
Json::Value HUDProfile(const HUDContext& ctx, const Encounter* encounter, bool forecast, const std::vector<MovementObject*>& nearby) {
    Json::Value result(Json::objectValue), actors(Json::arrayValue);
    const int originals = encounter ? static_cast<int>(encounter->originals.size()) : forecast || nearby.empty() ? 1 : static_cast<int>(nearby.size());
    const int expected = ctx.campaign && ctx.stats && encounter ? EncounterReinforcements(ctx.graph, *encounter, ctx.humans, ctx.bots) :
        ctx.campaign && ctx.stats && forecast ? CampaignDifficulty::ExtraEnemiesForParty(ctx.humans, ctx.bots, 1, false, CampaignDifficulty::Rabbit) : 0;
    const int spawned = !forecast && encounter ? encounter->spawned : 0;
    const bool deferred = !forecast && encounter && encounter->reinforcements_deferred;
    const int target = forecast ? expected : encounter ? std::max(spawned, deferred || !ctx.stats ? spawned : std::min(expected, encounter->reinforcement_target)) : 0;
    int alive = 0, extra_alive = 0, varied_ai = 0, eligible_ai = 0, armor_pieces = 0, armored_actors = 0, weapon_count = 0, weapon_tier = 0;
    double health_damage = 0;
    HUDFactors minimum = {{1e30f, 1e30f, 1e30f, 1e30f}}, maximum = {{0, 0, 0, 0}}, sum = {{0, 0, 0, 0}};
    auto add = [&](MovementObject* actor, bool boss, bool generated) {
        HUDContext reference = ctx;
        if (!forecast) reference.next_pressure = ctx.pressure;
        const HUDFactors values = forecast || !actor ? PredictedHUDFactors(reference, actor, boss, generated) : InstalledHUDFactors(actor);
        Json::Value row;
        row["id"] = actor && !generated ? ctx.online->GetOriginalID(actor->GetID()) : actor && !forecast ? ctx.online->GetOriginalID(actor->GetID()) : -1;
        row["vitality"] = values[0]; row["damage"] = values[1]; row["armor"] = values[2]; row["ai"] = values[3]; row["generated"] = generated;
        int mask = actor && !forecast && actor->ASHasVar("campaign_armor_regions") ? actor->ASGetIntVar("campaign_armor_regions") : 0;
        bool full_armor = actor && !generated && actor->ASHasVar("g_wearing_metal_armor") && actor->ASGetBoolVar("g_wearing_metal_armor");
        const int stock_mask = actor && !generated ? ctx.online->AuthoredArmorRegions(actor->GetID()) : 0;
        int armed = 0, tier = 0;
        if (forecast && encounter && ctx.online->DifficultyScalingEnabled()) {
            const int slot = generated ? int(actors.size()) - originals : 0;
            const uint32_t base = CampaignEquipment::ExtraSeed(ctx.online->GetOriginalID(encounter->originals.front()), ctx.online->GetOriginalID(encounter->hotspot), encounter->goal, 0);
            const uint32_t seed = generated ? CampaignEquipment::ExtraSeed(ctx.online->GetOriginalID(encounter->originals.front()), ctx.online->GetOriginalID(encounter->hotspot), encounter->goal, slot) : actor ? uint32_t(ctx.online->GetOriginalID(actor->GetID())) : 0;
            const double intensity = CampaignEquipment::Intensity(ctx.next_pressure, ctx.stats ? CampaignDifficulty::ExtraPlayersForParty(ctx.humans, ctx.bots) : 0);
            auto plan = CampaignEquipment::ForUnit(seed, intensity, config["campaign_armor_scaling"].toBool() && !(full_armor && (!config["armor_localized"].toBool() || !stock_mask)), false, false);
            mask = plan.armor_regions & ~stock_mask;
            if (generated && config["campaign_weapon_scaling"].toBool()) {
                tier = CampaignEquipment::WaveWeapon(base, slot, expected, intensity, actor && Strength(actor) == CampaignDifficulty::Wolf);
                armed = tier ? 1 : 0;
            }
        }
        if (actor && (!forecast || !generated)) {
            std::set<int> held;
            for (int i = 0; i < 4; ++i) {
                const int id = actor->ASGetArrayIntVar("weapon_slots", i);
                Object* item = id >= 0 ? ctx.graph->GetObjectFromID(id) : nullptr;
                if (!item || item->GetType() != _item_object || !held.insert(id).second) continue;
                const auto& asset = static_cast<ItemObject*>(item)->item_ref();
                if (asset->GetItemType() != _weapon) continue;
                const std::string label = asset->GetLabel();
                tier = std::max(tier, label == "spear" ? int(CampaignEquipment::Spear) : label == "sword" || label == "big_sword" || label == "rapier" ? int(CampaignEquipment::Sword) : label == "knife" ? int(CampaignEquipment::Dagger) : 0);
                ++armed;
            }
        }
        const int physical_pieces = CampaignEquipment::Pieces(mask) + CampaignEquipment::Pieces(stock_mask);
        row["armor_regions"] = mask; row["authored_armor_regions"] = stock_mask; row["armor_pieces"] = physical_pieces; row["authored_full_armor"] = full_armor;
        row["weapons"] = armed; row["weapon_tier"] = tier; row["weapon_label"] = armed && !tier ? "other" : HUDWeaponLabel(tier);
        armor_pieces += physical_pieces; if (mask || stock_mask || full_armor) ++armored_actors;
        weapon_count += armed; weapon_tier = std::max(weapon_tier, tier);
        const bool awake = forecast || (actor && actor->enabled_ && Awake(actor));
        row["alive"] = awake; actors.append(row);
        if (awake) { ++alive; if (generated) ++extra_alive; }
        if (values[3] > 0) {
            ++eligible_ai;
            if (forecast || (actor && actor->ASHasVar("campaign_ai_tweak") && actor->ASGetIntVar("campaign_ai_tweak") != 0)) ++varied_ai;
        }
        health_damage += double(values[0]) * values[1];
        for (int i = 0; i < 4; ++i) { minimum[i] = std::min(minimum[i], values[i]); maximum[i] = std::max(maximum[i], values[i]); sum[i] += values[i]; }
    };
    std::vector<MovementObject*> sources;
    if (encounter) {
        for (ObjectID id : encounter->originals) if (MovementObject* actor = Actor(ctx.graph, id)) {
            sources.push_back(actor); add(actor, encounter->bosses.count(id) != 0, false);
        }
        if (forecast && !sources.empty()) for (int i = 0; i < expected; ++i) {
            MovementObject* source = sources[i % sources.size()];
            if (!encounter->bosses.empty()) source = *std::min_element(sources.begin(), sources.end(), [](MovementObject* a, MovementObject* b) { return Strength(a) < Strength(b); });
            add(source, false, true);
        }
        if (!forecast) for (const auto& extra : encounter->extras) if (MovementObject* actor = Actor(ctx.graph, extra.actor)) add(actor, false, true);
    } else if (!forecast) for (MovementObject* actor : nearby) add(actor, ExplicitBoss(actor, false), false);
    if (actors.empty()) {
        // No scene actor is claimed by a reference estimate.
        add(nullptr, false, false);
        alive = 0;
        if (forecast) for (int i = 0; i < expected; ++i) add(nullptr, false, true);
    }
    const float possible_ai = forecast && ctx.ai_enabled && (expected > 0 || !encounter) ? CampaignDifficulty::AIAmount(ctx.humans, ctx.next_pressure) : maximum[3];
    const char* names[] = {"vitality", "damage", "armor", "ai"};
    for (int i = 0; i < 4; ++i) {
        result["factors"][names[i]]["min"] = minimum[i];
        result["factors"][names[i]]["max"] = i == 3 ? std::max(maximum[i], possible_ai) : maximum[i];
        result["factors"][names[i]]["mean"] = sum[i] / actors.size();
    }
    const int wave = forecast ? originals + expected : encounter ? originals + spawned : originals;
    result["known"] = encounter != nullptr;
    result["label"] = encounter ? (!encounter->bosses.empty() ? "Boss encounter" : "Encounter") : forecast ? "Next encounter (estimate)" : nearby.empty() ? "Reference enemy" : "Nearby enemies";
    result["hotspot"] = encounter ? ctx.online->GetOriginalID(encounter->hotspot) : -1;
    result["goal"] = encounter ? encounter->goal : -1;
    result["boss"] = encounter && !encounter->bosses.empty(); result["approximate"] = true;
    result["factor_source"] = forecast ? "forecast" : !encounter && nearby.empty() ? "reference" : "installed";
    result["reference"] = forecast && !encounter ? "regular_rabbit" : "";
    result["original_count"] = originals; result["actor_count"] = actors.size(); result["alive_count"] = alive;
    result["wave_count"] = wave; result["extras_expected"] = expected; result["extras_spawned"] = spawned;
    result["extras_target"] = target; result["extras_pending"] = std::max(0, target - spawned);
    result["extras_alive"] = extra_alive; result["extras_deferred"] = deferred;
    result["extras_reserved"] = encounter && !forecast ? encounter->reinforcement_target : target;
    result["ai_varied_actors"] = varied_ai; result["ai_eligible_actors"] = eligible_ai; result["actors"] = actors;
    result["armor_pieces"] = armor_pieces; result["armored_actors"] = armored_actors;
    result["weapons"] = weapon_count; result["weapon_tier"] = weapon_tier; result["weapon_label"] = weapon_count && !weapon_tier ? "other" : HUDWeaponLabel(weapon_tier);
    result["gear_source"] = forecast ? "planned estimate" : "applied";
    result["threat"] = double(wave) / std::max(1, originals) * health_damage / actors.size();
    return result;
}
std::string HUDNumber(double value, int precision = 2) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    std::string result = text.str();
    while (!result.empty() && result.back() == '0') result.pop_back();
    if (!result.empty() && result.back() == '.') result.pop_back();
    return result;
}
std::string HUDRange(const Json::Value& factors, const char* key) {
    const double minimum = factors[key]["min"].asDouble(), maximum = factors[key]["max"].asDouble();
    return std::fabs(maximum - minimum) < 0.005 ? HUDNumber(maximum) : HUDNumber(minimum, 1) + "-" + HUDNumber(maximum, 1);
}
std::string HUDMods(const Json::Value& profile) {
    const auto& factors = profile["factors"];
    return "HP x" + HUDRange(factors, "vitality") + " Dmg x" + HUDRange(factors, "damage");
}
std::string HUDGear(const Json::Value& profile) {
    return "Gear " + std::to_string(profile["armor_pieces"].asInt()) + " pieces | Weapons " + std::to_string(profile["weapons"].asInt()) + " (" + profile["weapon_label"].asString() + ")";
}
const char* HUDRate(int rate) { return rate == 1 ? "Gentle" : rate == 2 ? "Standard" : rate == 3 ? "Brisk" : "Off"; }
}

std::string Online::DifficultyHUDSnapshot() {
    Engine* engine = Engine::Instance();
    SceneGraph* graph = engine->GetSceneGraph();
    if (!difficulty_hud_state) difficulty_hud_state = std::make_shared<DifficultyHUDState>();
    auto& cache = *difficulty_hud_state;
    const double now = OnlineClockMs();
    if (cache.graph == graph && cache.level == level_name && now < cache.next_update && !cache.payload.empty()) return cache.payload;
    cache.graph = graph; cache.level = level_name; cache.next_update = now + 250.0;
    Json::Value result(Json::objectValue);
    result["version"] = 1;
    const bool scene = graph && graph->level && graph->map_editor && engine->IsLevelLoaded();
    const bool gameplay = scene && (!IsActive() || host_started_level) && graph->map_editor->state_ == MapEditor::kInGame && !loading && !LobbyWaiting() && !LobbyBuild() && !BuildSession();
    const bool authoritative = !IsActive() || IsHosting();
    result["authoritative"] = authoritative;
    result["base"]["name"] = config.GetDifficultyPreset().empty() ? "Custom" : config.GetDifficultyPreset();
    result["base"]["game_difficulty"] = config["game_difficulty"].toNumber<float>();
    result["base"]["time_scale"] = config["global_time_scale_mult"].toNumber<float>();
    DescribeChallengeHUD(result["progression"]);
    HUDContext ctx;
    ctx.online = this; ctx.graph = graph;
    ctx.campaign = IsActive() && CoopCampaign();
    const bool master = DifficultyScalingEnabled();
    ctx.stats = ctx.campaign && master && config["campaign_mp_scaling"].toBool();
    ctx.ai_enabled = master && config["campaign_ai_scaling"].toBool();
    ctx.pressure = result["progression"]["pressure"].asDouble();
    ctx.next_pressure = result["progression"]["next_pressure"].asDouble();
    MovementObject* reference = scene ? Actor(graph, ChallengeReferenceAvatar()) : nullptr;
    if (!reference && scene) for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        if (actor->controlled) { reference = actor; break; }
    }
    ctx.humans = 0;
    if (IsActive() && online_session && scene) for (const auto& entry : online_session->player_states) {
        MovementObject* actor = Actor(graph, entry.second.object_id);
        if (!actor || (ctx.campaign ? !CampaignPlayerOnMainTeam(entry.first) : reference && !actor->ASOnSameTeam(reference))) continue;
        if (entry.second.is_bot) ++ctx.bots; else ++ctx.humans;
    }
    if (!IsActive() && reference) ctx.humans = 1;
    result["campaign"] = ctx.campaign;
    result["party"]["humans"] = ctx.humans; result["party"]["bots"] = ctx.bots;
    result["party"]["stats_enabled"] = ctx.stats;
    result["party"]["stats_weight"] = CampaignDifficulty::EffectivePartySize(ctx.humans, ctx.bots);
    result["party"]["count_weight"] = std::max(1, std::min(64, ctx.humans)) + 0.5 * std::min(64, ctx.bots);
    result["party"]["ai_humans"] = ctx.humans;
    result["party"]["ai_enabled"] = ctx.ai_enabled;
    result["equipment"]["armor_enabled"] = master && config["campaign_armor_scaling"].toBool();
    result["equipment"]["weapons_enabled"] = master && config["campaign_weapon_scaling"].toBool();
    result["equipment"]["localized"] = config["armor_localized"].toBool();
    const Encounter* current = nullptr;
    const Encounter* next = nullptr;
    float current_distance = 1e30f;
    bool retained_extras = false;
    if (scene && campaign_difficulty_state && campaign_difficulty_state->graph == graph && !campaign_difficulty_state->reset) {
        const auto& state = *campaign_difficulty_state;
        for (const auto& entry : state.encounters) {
            const Encounter& encounter = entry.second;
            for (const auto& extra : encounter.extras) if (MovementObject* actor = Actor(graph, extra.actor))
                if (actor->enabled_ && Awake(actor)) retained_extras = true;
            Object* object = graph->GetObjectFromID(encounter.hotspot);
            if (!object || object->GetType() != _hotspot_object || encounter.originals.empty()) continue;
            const int progress = static_cast<Hotspot*>(object)->ASGetIntVar("progress");
            if (progress != encounter.goal) continue;
            float nearest = 1e30f;
            for (ObjectID id : encounter.originals) if (MovementObject* actor = Actor(graph, id))
                nearest = std::min(nearest, reference ? distance_squared(reference->position, actor->position) : 0.0f);
            if (!current || nearest < current_distance) { current = &encounter; current_distance = nearest; }
        }
        ObjectID preview_hotspot = current ? current->hotspot : -1;
        bool ambiguous = false;
        if (!current) for (const auto& entry : state.encounters) {
            const Encounter& encounter = entry.second;
            Object* object = graph->GetObjectFromID(encounter.hotspot);
            if (!object || object->GetType() != _hotspot_object || encounter.originals.empty() ||
                encounter.goal <= static_cast<Hotspot*>(object)->ASGetIntVar("progress")) continue;
            if (preview_hotspot < 0) preview_hotspot = encounter.hotspot;
            else if (preview_hotspot != encounter.hotspot) ambiguous = true;
        }
        // Authored hotspot goal order is meaningful; IDs of unrelated
        // hotspots are not. Restrict a known preview to the current hotspot.
        for (const auto& entry : state.encounters) {
            const Encounter& encounter = entry.second;
            Object* object = graph->GetObjectFromID(encounter.hotspot);
            if (!object || object->GetType() != _hotspot_object || encounter.originals.empty() ||
                ambiguous || preview_hotspot != encounter.hotspot) continue;
            const int progress = static_cast<Hotspot*>(object)->ASGetIntVar("progress");
            if (encounter.goal <= progress) continue;
            if (!next || encounter.goal < next->goal) next = &encounter;
        }
    }
    std::vector<MovementObject*> nearby;
    bool eligible_npcs = false, retained_factors = false;
    if (scene) for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        if (actor->controlled || (IsPlayerCharacter(actor->GetID()) && !LobbyBot(this, actor->GetID())) ||
            !actor->HasFunction("void SetCampaignScaling(float, float, float, float)")) continue;
        const bool hostile = IsActive() ? Hostile(this, graph, actor) : reference && !actor->ASOnSameTeam(reference);
        if (hostile) {
            eligible_npcs = true;
            if (actor->enabled_ && Awake(actor) && (!reference || distance_squared(actor->position, reference->position) < 55.0f * 55.0f)) nearby.push_back(actor);
        }
        if (actor->enabled_ && Awake(actor)) {
            const auto factors = InstalledHUDFactors(actor);
            retained_factors = retained_factors || std::fabs(factors[0] - 1) > 0.0001f || std::fabs(factors[1] - 1) > 0.0001f ||
                factors[3] > 0.0001f || (actor->ASHasVar("campaign_armor_regions") && actor->ASGetIntVar("campaign_armor_regions") != 0);
        }
    }
    result["current"] = HUDProfile(ctx, current, false, nearby);
    result["next"] = HUDProfile(ctx, next, true, {});
    const bool arena = LobbyMode() == "com-wolfire-arena" || LobbyCampaign() == "com-wolfire-arena";
    const auto& rates = result["progression"]["rates"];
    const int completion_rate = result["progression"]["completion_supported"].asBool() ? rates["completion"].asInt() : 0;
    const int performance_mode = result["progression"]["performance"]["mode"].asInt();
    const int performance_rate = result["progression"]["performance"]["rate"].asInt();
    const bool growth = master && (rates["time"].asInt() || (performance_mode && performance_rate) || completion_rate);
    const bool party_stats = ctx.stats && ctx.humans + ctx.bots > 1;
    const bool ai = ctx.ai_enabled && CampaignDifficulty::AIAmount(ctx.humans, ctx.pressure) > 0;
    const bool scaled = retained_factors || retained_extras || ((eligible_npcs || arena) && (party_stats || ai || growth || ctx.pressure > 0.00001));
    result["scaled_context"] = scaled;
    result["visible"] = authoritative && gameplay && scaled && !CoopCutscene() && !RemoteCutsceneActive() && !graph->level->DialogueCameraControl();
    result["rating"]["current"] = result["current"]["threat"];
    result["rating"]["next"] = result["next"]["threat"];
    result["rating"]["approximate"] = true;
    result["rating"]["relative_to"] = "Same authored encounter at the selected base preset, without party/progression scaling";
    result["rating"]["formula"] = "wave count / authored count * mean(vitality * damage); excludes physical gear, AI, terrain and player enemies";
    result["rating"]["wave_includes_defeated"] = true;
    result["next"]["preview_basis"] = result["progression"]["preview_basis"];
    if (!next) result["next"]["preview_basis"] = "Regular rabbit reference, not a discovered upcoming wave; " + result["progression"]["preview_basis"].asString();
    Json::Value lines(Json::arrayValue);
    lines.append(result["base"]["name"].asString() + " | Threat ~" + HUDNumber(result["current"]["threat"].asDouble(), 1) + "x base");
    std::string party = std::to_string(ctx.humans) + (ctx.humans == 1 ? " human + " : " humans + ") +
        std::to_string(ctx.bots) + (ctx.bots == 1 ? " bot" : " bots");
    party += ctx.stats ? " | Party scaling on" : " | Party scaling off";
    lines.append(party);
    const auto& live = result["current"];
    std::string current_line = std::string(live["factor_source"].asString() == "reference" ? "Now est " : live["boss"].asBool() ? "Now boss " : "Now ") + HUDMods(live);
    if (current) current_line += " | +" + std::to_string(live["extras_spawned"].asInt()) + " enemies";
    if (live["extras_pending"].asInt() > 0) current_line += " (" + std::to_string(live["extras_pending"].asInt()) + " pending)";
    current_line += " | " + HUDGear(live);
    if (live["ai_varied_actors"].asInt() > 0) current_line += " AI+" + HUDNumber(live["factors"]["ai"]["max"].asDouble() * 100, 1) + "%";
    lines.append(current_line);
    std::string next_line = std::string(next ? "Next " : "Next est (rabbit) ") + "~" + HUDNumber(result["next"]["threat"].asDouble(), 1) + "x | " + HUDMods(result["next"]);
    if (next) next_line += " | +" + std::to_string(result["next"]["extras_expected"].asInt()) + " enemies";
    lines.append(next_line);
    std::string growth_line;
    auto driver = [&](const std::string& label, int rate) {
        if (rate) { if (!growth_line.empty()) growth_line += " | "; growth_line += label + " " + HUDRate(rate); }
    };
    if (master) {
        driver("Time", rates["time"].asInt()); driver("Levels", completion_rate);
        driver(performance_mode == 1 ? "On win" : performance_mode == 2 ? "On loss" : "Forced 50/50", performance_mode ? performance_rate : 0);
        if (ctx.ai_enabled) growth_line += (growth_line.empty() ? "" : " | ") + std::string("AI gentle");
    }
    lines.append("Growth: " + (growth_line.empty() ? "Off" : growth_line));
    const auto& progress = result["progression"];
    std::string pressure_line = "Pressure " + std::string(ctx.pressure >= 0 ? "+" : "") + HUDNumber(ctx.pressure * 100, 1) + "%";
    if (progress["win_pending"].asBool()) pressure_line += " -> " + std::string(ctx.next_pressure >= 0 ? "+" : "") + HUDNumber(ctx.next_pressure * 100, 1) + "% on win";
    if (progress["encounter_relief"].asDouble() > 0) pressure_line += " | Retry relief " + HUDNumber(progress["encounter_relief"].asDouble() * 100, 1) + "%";
    else pressure_line += " | " + std::to_string(static_cast<int>(progress["active_seconds"].asDouble() / 60)) + "m active";
    lines.append(pressure_line);
    result["lines"] = lines;
    cache.payload = Json::FastWriter().write(result);
    return cache.payload;
}

int Online::AuthoredArmorRegions(ObjectID avatar) const {
    MovementObject* actor = Actor(Engine::Instance()->GetSceneGraph(), avatar);
    if (!actor || !actor->rigged_object()) return 0;
    int mask = 0;
    for (const auto& child : actor->rigged_object()->children) mask |= AuthoredPieceRegions(actor, child.direct_ptr, child);
    return mask;
}
