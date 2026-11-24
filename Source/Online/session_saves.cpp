// Host-local session saves. The manifest is replaced only after the thumbnail
// and the complete replacement manifest have been written successfully.
#include <Online/session_saves.h>
#include <Online/online.h>
#include <Online/friendly_progression.h>
#include <Online/Message/set_object_enabled_message.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Game/scriptablecampaign.h>
#include <Game/level.h>
#include <Editors/map_editor.h>
#include <Graphics/graphics.h>
#include <Images/stbimage_wrapper.h>
#include <Internal/modloading.h>
#include <Graphics/local_viewport.h>
#include <UserInput/input.h>
#include <Internal/filesystem.h>
#include <Compat/fileio.h>
#include <Objects/hotspot.h>
#include <Objects/movementobject.h>
#include <JSON/json.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

namespace SessionSaves {
namespace {
constexpr int kWidth = 256, kHeight = 144;
constexpr size_t kMaxBytes = 16 * 1024 * 1024;
enum class Stage { Idle,
                   OpenLobby,
                   WaitingForStart,
                   LoadWorld };
Stage stage = Stage::Idle;
Json::Value pending, arena_points, arena_restore, arena_state, campaign_transition;
int arena_round = 0;
bool initial_save = false;
bool client_scene = false;
bool ledger_applied = false;
std::set<int> requested;
std::string status;
using Clock = std::chrono::steady_clock;
Clock::time_point scene_ready;
bool Slot(int slot) { return slot >= 0 && slot < kSlots; }
std::string Base() { return GetWritePath(CoreGameModID); }
std::string Manifest(int slot) { return Base() + "Data/SessionSaves/slot" + std::to_string(slot) + ".json"; }
std::string Image(int slot, const Json::Value& record) {
    return "Textures/SessionSaves/slot" + std::to_string(slot) + "-" + record["image_id"].asString() + ".jpg";
}
std::string MapPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto at = path.find("Data/Levels/");
    if (at != std::string::npos) path.erase(0, at);
    return path;
}
bool ValidMap(const std::string& path) {
    return path.size() > 16 && path.size() <= 4096 && path.compare(0, 12, "Data/Levels/") == 0 &&
           path.compare(path.size() - 4, 4, ".xml") == 0 && path.find("..") == std::string::npos &&
           path.find_first_of("\r\n\\:") == std::string::npos && path.find('\0') == std::string::npos;
}
bool Read(int slot, Json::Value& record) {
    if (!Slot(slot)) return false;
    FILE* file = my_fopen(Manifest(slot).c_str(), "rb");
    if (!file) return false;
    std::string data;
    char buffer[8192];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0 && data.size() <= kMaxBytes) data.append(buffer, count);
    const bool ok = !ferror(file);
    fclose(file);
    if (!ok || data.size() > kMaxBytes || !Json::Reader().parse(data, record, false) ||
        !record.isObject() || record.get("version", 0) != 1 || !record["map"].isString() ||
        !ValidMap(record["map"].asString()) || !record["mode"].isString() || !record["campaign"].isString() ||
        !record["lobby"].isBool() || !record["date"].isString() || !record["image_id"].isString() ||
        !record["rules"].isObject() || !record["world"].isObject() || !record["progression"].isObject()) return false;
    if (record["mode"].asString().size() > 256 || record["campaign"].asString().size() > 256 ||
        (record.isMember("local_only") && !record["local_only"].isBool())) return false;
    const std::string id = record["image_id"].asString();
    return !id.empty() && id.size() <= 24 && id.find_first_not_of("0123456789") == std::string::npos;
}
bool WorldReady() {
    auto* engine = Engine::Instance();
    auto* graph = engine->GetSceneGraph();
    return engine->IsLevelLoaded() && graph && graph->level && graph->map_editor &&
           graph->map_editor->state_ == MapEditor::kInGame &&
           ValidMap(MapPath(engine->GetLatestLevelPath().GetOriginalPathStr()));
}
std::string CurrentMap() { return MapPath(Engine::Instance()->GetLatestLevelPath().GetOriginalPathStr()); }
Json::Value Vector(const vec3& v) {
    Json::Value out(Json::arrayValue);
    for (int i = 0; i < 3; ++i) out.append(v[i]);
    return out;
}
bool ValidVec(const Json::Value& v) {
    if (!v.isArray() || v.size() != 3) return false;
    for (const auto& n : v)
        if (!n.isNumeric() || !std::isfinite(n.asDouble()) || std::abs(n.asDouble()) > 1000000) return false;
    return true;
}
bool ValidArenaState(const Json::Value& value) {
    if (!value.isObject() || value.get("version", 0) != 1 || !value["round"].isInt() ||
        value["round"].asInt() < 1 || value["round"].asInt() > 1000000 ||
        !value["phase"].isInt() || value["phase"].asInt() < 0 || value["phase"].asInt() > 6 ||
        !value["max_points"].isInt() || value["max_points"].asInt() < 1 || value["max_points"].asInt() > 1000000 ||
        !value["match_size"].isInt() || value["match_size"].asInt() < 1 || value["match_size"].asInt() > 4096 ||
        !value["weapons"].isBool() || !value["ffa"].isBool()) return false;
    for (const char* key : {"battle", "intro"})
        if (!value[key].isString() || value[key].asString().size() > 4096) return false;
    for (const char* key : {"challenge_match", "challenge_round"})
        if (!value[key].isString() || value[key].asString().size() > 4096) return false;
    if (!value["challenge_serial"].isInt() || value["challenge_serial"].asInt() < 0 ||
        value["challenge_serial"].asInt() > 1000000) return false;
    for (const char* key : {"spots", "items", "weights", "fighters"})
        if (!value[key].isArray() || value[key].size() > 4096) return false;
    if (value["spots"].empty() || value["fighters"].empty() || value["weights"].empty()) return false;
    auto path = [](const Json::Value& p) {
        if (!p.isString()) return false;
        const auto s = p.asString();
        return s.empty() || (s.size() < 4096 && s.compare(0, 11, "Data/Items/") == 0 &&
                            s.size() > 15 && s.compare(s.size() - 4, 4, ".xml") == 0 &&
                            s.find("..") == std::string::npos && s.find_first_of("\r\n\\:") == std::string::npos);
    };
    for (const auto& spot : value["spots"])
        if (!spot.isObject() || !ValidVec(spot["pos"]) || !path(spot["weapon"]) || !spot["team"].isInt() ||
            spot["team"].asInt() < 0 || spot["team"].asInt() > 4096) return false;
    for (const auto& item : value["items"])
        if (!item.isObject() || !ValidVec(item["pos"]) || !path(item["path"])) return false;
    for (const auto& weight : value["weights"])
        if (!weight.isInt() || weight.asInt() < 1 || weight.asInt() > 4096) return false;
    std::set<std::string> keys;
    for (const auto& fighter : value["fighters"]) {
        if (!fighter.isObject() || !fighter["key"].isString() || !fighter["points"].isInt() || fighter["points"].asInt() < 0 ||
            fighter["points"].asInt() > 1000000 || !fighter["team"].isInt() || fighter["team"].asInt() < 1 ||
            fighter["team"].asInt() > 4096 || !fighter["spot"].isInt() || fighter["spot"].asInt() < 0 ||
            fighter["spot"].asInt() > 1000000 || !fighter["winner"].isBool()) return false;
        const auto key = fighter["key"].asString();
        if (key.size() < 5 || key.size() > 24 || (key.compare(0, 4, "seat") && key.compare(0, 4, "fill")) ||
            key.find_first_not_of("0123456789", 4) != std::string::npos || !keys.insert(key).second) return false;
    }
    return true;
}
bool ValidWorld(const Json::Value& world) {
    const auto& actors = world["actors"];
    const auto& goals = world["goals"];
    const auto& points = world["points"];
    if ((!actors.isNull() && !actors.isObject()) || actors.size() > 8192 ||
        (!goals.isNull() && !goals.isObject()) || goals.size() > 4096 ||
        (!points.isNull() && !points.isObject()) || points.size() > 8192 ||
        !world["round"].isInt() || world["round"].asInt() < 0 || world["round"].asInt() > 1000000) return false;
    if (!world["arena"].isNull() && !ValidArenaState(world["arena"])) return false;
    const auto& transition = world["campaign_transition"];
    if (!transition.isNull() && (!transition.isString() || transition.asString().size() > 4096 ||
        transition.asString().find_first_of("\r\n") != std::string::npos || transition.asString().find('\0') != std::string::npos)) return false;
    for (const auto& score : points)
        if (!score.isInt() || score.asInt() < 0 || score.asInt() > 1000000) return false;
    for (const auto& goal : goals)
        if (!goal.isString() || goal.asString().size() > 65536 ||
            goal.asString().find_first_not_of("0123456789 -") != std::string::npos) return false;
    for (const auto& actor : actors) {
        if (!actor.isObject() || !actor["enabled"].isBool() || !actor["ko"].isInt() ||
            actor["ko"].asInt() < 0 || actor["ko"].asInt() > 2 ||
            !ValidVec(actor["pos"]) || !ValidVec(actor["facing"])) return false;
        for (const char* key : {"temp_health", "blood_health", "permanent_health"}) {
            const auto& value = actor[key];
            if (!value.isNull() && (!value.isNumeric() || !std::isfinite(value.asDouble()) || std::abs(value.asDouble()) > 1000000)) return false;
        }
    }
    return true;
}
vec3 Vec(const Json::Value& v) { return vec3(v[0].asFloat(), v[1].asFloat(), v[2].asFloat()); }
std::string ActorKey(MovementObject* actor) {
    auto* net = Online::Instance();
    PlayerState seat;
    if (net->IsActive() && net->TryGetPlayerState(seat, actor->GetID())) return "seat" + std::to_string(seat.player_id);
    if (!net->IsActive() && actor->controlled) return "solo" + std::to_string(actor->controller_id);
    if (actor->exclude_from_save || actor->created_on_the_fly) return {};
    return "actor" + std::to_string(net->IsActive() ? net->GetOriginalID(actor->GetID()) : actor->GetID());
}
Json::Value CaptureWorld(SceneGraph* graph) {
    Json::Value world(Json::objectValue);
    for (auto* obj : graph->hotspots_) {
        auto* hotspot = static_cast<Hotspot*>(obj);
        if (hotspot->HasScript() && hotspot->HasFunction("string SessionCheckpointState()"))
            world["goals"][std::to_string(hotspot->GetID())] = hotspot->QueryStringFunction("string SessionCheckpointState()");
    }
    for (auto* obj : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(obj);
        const auto key = ActorKey(actor);
        if (key.empty() || !actor->ASHasVar("knocked_out")) continue;
        Json::Value row;
        row["pos"] = Vector(actor->position);
        row["facing"] = Vector(actor->GetFacing());
        if (!ValidVec(row["pos"]) || !ValidVec(row["facing"])) continue;
        row["enabled"] = actor->enabled_;
        row["ko"] = actor->ASGetIntVar("knocked_out");
        for (const char* stat : {"temp_health", "blood_health", "permanent_health"}) {
            if (actor->ASHasVar(stat)) row[stat] = actor->ASGetFloatVar(stat);
        }
        world["actors"][key] = row;
    }
    world["cinematic"] = graph->level->DialogueCameraControl();
    world["round"] = arena_round;
    world["points"] = arena_points;
    if (!arena_state.isNull()) world["arena"] = arena_state;
    if (!campaign_transition.isNull()) world["campaign_transition"] = campaign_transition;
    return world;
}
void RestoreWorld(const Json::Value& world) {
    auto* net = Online::Instance();
    auto* graph = Engine::Instance()->GetSceneGraph();
    for (auto* obj : graph->hotspots_) {
        auto* h = static_cast<Hotspot*>(obj);
        const auto& saved = world["goals"][std::to_string(h->GetID())];
        if (saved.isString() && h->HasScript() && h->HasFunction("string SessionCheckpointState()")) {
            const std::string value = saved.asString();
            if (value.find_first_not_of("0123456789 -") == std::string::npos)
                h->ReceiveScriptMessage("session_restore " + value);
        }
    }
    for (auto* obj : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(obj);
        auto key = ActorKey(actor);
        const auto& actors = world["actors"];
        if (!actors.isMember(key) && net->IsHosting() && actor->GetID() == net->CampaignMainAvatarID()) key = "solo0";
        const auto& row = actors[key];
        if (!row.isObject() || !row["enabled"].isBool() || !row["ko"].isInt() ||
            !ValidVec(row["pos"]) || !ValidVec(row["facing"]) || !actor->ASHasVar("knocked_out")) continue;
        const vec3 position = Vec(row["pos"]), facing = Vec(row["facing"]);
        if (net->IsHosting() && net->IsPlayerCharacter(actor->GetID()) && row["enabled"].asBool()) {
            Online::SavedVitals vitals;
            vitals.knocked_out = row["ko"].asInt();
            vitals.temp_health = row.get("temp_health", 1.0f).asFloat();
            vitals.blood_health = row.get("blood_health", 1.0f).asFloat();
            vitals.permanent_health = row.get("permanent_health", 1.0f).asFloat();
            net->PlaceAvatar(actor->GetID(), position, facing, -1, &vitals);
        }
        actor->position = position;
        actor->velocity = vec3(0);
        if (length_squared(facing) > .001f) actor->SetRotationFromFacing(facing);
        if (actor->HasFunction("void FixDiscontinuity()")) actor->Execute("FixDiscontinuity();");
        obj->SetEnabled(row["enabled"].asBool());
        if (net->IsHosting()) net->Send<OnlineMessages::SetObjectEnabledMessage>(actor->GetID(), actor->enabled_);
        const int ko = row["ko"].asInt();
        if (ko > 0 && ko <= 2 && actor->HasFunction("void Ragdoll(RagdollType)")) actor->Execute("Ragdoll(_RGDL_LIMP);");
        actor->ASSetIntVar("knocked_out", ko);
        for (const char* stat : {"temp_health", "blood_health", "permanent_health"})
            if (row[stat].isNumeric() && std::isfinite(row[stat].asDouble()) && actor->ASHasVar(stat))
                actor->ASSetFloatVar(stat, row[stat].asFloat());
    }
}
std::string Now() {
    const time_t t = std::time(nullptr);
    char date[40] = {};
    const std::tm* local = std::localtime(&t);
    if (local) std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", local);
    return date;
}
bool WriteThumbnail(const std::string& path, const std::vector<unsigned char>& rgb) {
    CreateParentDirs(path);
    FILE* file = my_fopen(path.c_str(), "wb");
    if (!file) return false;
    auto sink = [](void* dst, void* bytes, int n) { fwrite(bytes, 1, n, static_cast<FILE*>(dst)); };
    bool ok = stbi_write_jpg_to_func(sink, file, kWidth, kHeight, 3, rgb.data(), 80) != 0 && !ferror(file);
    return fclose(file) == 0 && ok;
}
bool Commit(int slot, Json::Value record, const std::vector<unsigned char>& rgb) {
    Json::Value old;
    const bool previous = Read(slot, old);
    auto nonce = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    record["image_id"] = std::to_string(nonce);
    if (previous && old["image_id"] == record["image_id"]) record["image_id"] = std::to_string(nonce + 1);
    const std::string picture = Base() + "Data/" + Image(slot, record);
    if (!WriteThumbnail(picture, rgb)) {
        deletefile(picture.c_str());
        return false;
    }
    const auto path = Manifest(slot), temp = path + ".tmp";
    CreateParentDirs(path);
    FILE* file = my_fopen(temp.c_str(), "wb");
    bool ok = false;
    if (file) {
        const std::string data = Json::FastWriter().write(record);
        ok = data.size() <= kMaxBytes && fwrite(data.data(), 1, data.size(), file) == data.size();
        ok = fclose(file) == 0 && ok;
    }
    if (!ok || movefile(temp.c_str(), path.c_str()) != 0) {
        deletefile(temp.c_str());
        deletefile(picture.c_str());
        return false;
    }
    if (previous) deletefile((Base() + "Data/" + Image(slot, old)).c_str());
    return true;
}
}  // namespace

bool Exists(int slot) {
    Json::Value r;
    return Read(slot, r);
}
std::string Card(int slot, const std::string& field) {
    Json::Value r;
    if (!Read(slot, r)) return {};
    if (field == "image") return Image(slot, r);
    if (field == "mode") return r["mode_name"].isString() ? r["mode_name"].asString() : r["mode"].asString();
    if (field == "map") return r["map_name"].isString() ? r["map_name"].asString() : r["map"].asString();
    return field == "date" ? r["date"].asString() : "";
}
std::string Status() { return status; }
bool CanWrite() { return !client_scene && !Online::Instance()->IsClient() && WorldReady() && stage == Stage::Idle; }
bool Save(int slot) {
    if (!CanWrite() || slot < 1 || !Slot(slot)) return false;
    requested.insert(slot);
    status = "Saving slot " + std::to_string(slot) + "…";
    return true;
}
bool ResumingGameplay() {
    return stage == Stage::LoadWorld && pending["world"]["cinematic"] == false;
}
void SetCampaignTransition(bool waiting, const std::string& destination) {
    if (Online::Instance()->IsClient() || destination.size() > 4096 ||
        destination.find_first_of("\r\n") != std::string::npos || destination.find('\0') != std::string::npos) return;
    campaign_transition = waiting ? Json::Value(destination) : Json::Value();
    if (waiting) Autosave();
}
bool CampaignTransition(std::string& destination) {
    destination = campaign_transition.isString() ? campaign_transition.asString() : "";
    return !Online::Instance()->IsClient() && campaign_transition.isString();
}
void Autosave() {
    if (stage == Stage::Idle && !client_scene && !Online::Instance()->IsClient()) requested.insert(0);
}
void SceneLoaded() {
    client_scene = Online::Instance()->IsClient();
    initial_save = !client_scene;
    scene_ready = Clock::now();
}
void SceneClosed() {
    requested.clear();
    initial_save = false;
    arena_round = 0;
    arena_points.clear();
    arena_state = Json::Value();
    campaign_transition = Json::Value();
}
void LeaveLobby() {
    stage = Stage::Idle;
    pending.clear();
    ledger_applied = false;
    arena_restore.clear();
}
bool ResumeLobby(const std::string& mode, const std::string& campaign, const std::string& map, bool build) {
    if (stage != Stage::WaitingForStart) return false;
    if (mode != pending["mode"].asString() || campaign != pending["campaign"].asString() || MapPath(map) != pending["map"].asString() || build != pending["rules"]["build"].asBool()) {
        LeaveLobby();
        FriendlyProgression::NewSession();
        return false;
    }
    // Keep roster edits made in the restored lobby, including removal of a unit.
    const auto edited = FriendlyProgression::Snapshot();
    pending["progression"]["players"] = edited["players"];
    pending["progression"]["teams"] = edited["teams"];
    stage = Stage::LoadWorld;
    arena_restore = pending["world"];
    return true;
}
bool Load(int slot) {
    auto* net = Online::Instance();
    if (net->IsClient() || net->LobbyMigrationBusy()) return false;
    Json::Value r;
    if (!Read(slot, r) || !FindFilePath(r["map"].asString(), kAnyPath, false).isValid()) {
        status = "The save or its map is unavailable.";
        return false;
    }
    if (r["world"]["arena"].isObject() && r["world"]["arena"]["phase"].isInt() &&
        r["world"]["arena"]["phase"].asInt() == 5) {
        // Older cards opened a shop after every round. Resume that awarded
        // round at its normal boundary, with shopping reserved for match end.
        r["world"]["arena"]["phase"] = 3;
        r["progression"]["open"] = false;
        r["progression"]["ms_left"] = -1.0;
        r["progression"]["ready"] = Json::Value(Json::arrayValue);
    }
    std::string campaign_id = r["campaign"].asString();
    if (!campaign_id.empty() && ModLoading::Instance().GetCampaign(campaign_id).id.str().empty()) {
        status = "The saved campaign is unavailable.";
        return false;
    }
    const auto& transition = r["world"]["campaign_transition"];
    if (!transition.isNull()) {
        bool valid = !campaign_id.empty() && transition.isString() && transition.asString().empty();
        if (transition.isString()) for (const auto& level : ModLoading::Instance().GetCampaign(campaign_id).levels)
            if (level.id.str() == transition.asString()) valid = true;
        if (!valid) { status = "The saved campaign transition is unavailable."; return false; }
    }
    const auto& rules = r["rules"];
    if (!ValidWorld(r["world"]) || !net->ValidateGameRules(rules) || !FriendlyProgression::Restore(r["progression"], true)) {
        status = "The save contains invalid game settings or progression.";
        return false;
    }
    if (r["lobby"].asBool() && !net->ValidateLobbySlots(r["slots"])) {
        status = "The saved roster cannot accommodate the connected players.";
        return false;
    }
    requested.clear();
    ledger_applied = false;
    pending = std::move(r);
    const bool lobby = pending["lobby"].asBool() || (net->IsActive() && !net->IsLocalOnlySession());
    if (lobby) {
        const bool local_only = pending.get("local_only", false).asBool();
        if (!net->IsActive() && !net->BeginLobby(local_only)) {
            LeaveLobby();
            status = "Could not open the saved lobby.";
            return false;
        }
        if (net->IsLocalOnlySession() && pending["lobby"].asBool() && !local_only &&
            !net->OpenLobbyToNetwork(net->LobbyHostPlayerID())) {
            LeaveLobby();
            status = "Could not open the saved network lobby.";
            return false;
        }
        stage = Stage::OpenLobby;
        if (net->LobbyWaiting())
            Engine::Instance()->ReturnToLobby();
        else if (!net->EndLobbyGame(net->LobbyHostPlayerID())) {
            LeaveLobby();
            status = "Could not leave the current match.";
            return false;
        }
    } else {
        stage = Stage::LoadWorld;
        arena_restore = pending["world"];
        net->RestoreGameRules(pending["rules"]);
        const auto campaign = pending["campaign"].asString();
        if (!campaign.empty()) Engine::Instance()->ScriptableUICallback("set_campaign " + campaign);
        auto lookup = campaign;
        std::string level_id;
        if (!lookup.empty())
            for (const auto& entry : ModLoading::Instance().GetCampaign(lookup).levels)
                if (MapPath("Data/Levels/" + entry.path.str()) == pending["map"].asString()) level_id = entry.id.str();
        Engine::Instance()->ScriptableUICallback(level_id.empty() ? pending["map"].asString() : "load_campaign_level " + level_id);
        // The old scene may still be live until the queued load runs.
        scene_ready = Clock::time_point::max();
    }
    status.clear();
    return true;
}
void Update() {
    auto* engine = Engine::Instance();
    auto* net = Online::Instance();
    if (stage == Stage::OpenLobby && net->LobbyWaiting() && !engine->IsLevelLoaded()) {
        if (pending["lobby"].asBool() && !net->RestoreLobbySlots(pending["slots"])) {
            status = "The saved slots could not be restored.";
            LeaveLobby();
            return;
        }
        net->RestoreGameRules(pending["rules"]);
        net->SetLobbySelection(pending["mode"].asString(), pending["campaign"].asString(), pending["map"].asString(), pending["rules"]["build"].asBool(), net->LobbyHostPlayerID());
        FriendlyProgression::Restore(pending["progression"]);
        stage = Stage::WaitingForStart;
        net->BroadcastLobby();
    }
    if (!WorldReady() || engine->waiting_for_input_ || net->IsClient()) return;
    if (stage == Stage::LoadWorld && scene_ready != Clock::time_point::max() &&
        CurrentMap() == pending["map"].asString() && (!net->IsHosting() || net->host_started_level)) {
        if (!ledger_applied) {
            // Level entry resets pending XP and shopping time. Restore these once,
            // after deployment, including when Arena is waiting for shoppers.
            if (!pending["lobby"].asBool()) net->RestoreGameRules(pending["rules"]);
            FriendlyProgression::Restore(pending["progression"]);
            ledger_applied = true;
        }
        if (arena_restore.get("round", 0).asInt() > 0 && arena_round == 0) return;
        if (arena_round == 0) {
            auto* level = engine->GetSceneGraph()->level;
            if (pending["world"]["cinematic"] == false && level->DialogueCameraControl()) {
                level->Message("skip_dialogue");
                return;
            }
            RestoreWorld(pending["world"]);
            // Install only after level entry has reset the campaign locals and
            // the saved shop ledger is in place. Finishing it consumes this once.
            campaign_transition = pending["world"]["campaign_transition"];
        }
        if (net->IsHosting()) net->BroadcastLobby();
        LeaveLobby();
        initial_save = false;
    }
    if (stage == Stage::Idle && initial_save && Clock::now() - scene_ready > std::chrono::seconds(1)) {
        initial_save = false;
        Autosave();
    }
}
void CaptureFrame() {
    if (requested.empty() || !CanWrite() || Engine::Instance()->waiting_for_input_) return;
    auto* engine = Engine::Instance();
    auto* net = Online::Instance();
    auto* graph = engine->GetSceneGraph();
    auto* g = Graphics::Instance();
    const int w = g->window_dims[0], h = g->window_dims[1];
    if (w <= 0 || h <= 0) return;
    // Read the gameplay image before menus/HUD. Pick the host's viewport in a couch game.
    int x = 0, y = 0, width = w, height = h;
    if (net->IsHosting()) {
        const auto locals = net->GetLocalPlayerStates();
        const int screens = std::max(1u, Input::Instance()->GetNumLocalPlayers());
        for (const auto& player : locals)
            if (net->IsLobbyHost(player.player_id) && player.screen > 0 && player.screen <= screens) {
                const auto viewport = LocalViewports::ForPlayer(engine->GetSplitScreen() ? player.screen - 1 : 0, engine->GetSplitScreen() ? screens : 1);
                x = int(viewport.left * w);
                y = int(viewport.bottom * h);
                width = int(viewport.right * w) - x;
                height = int(viewport.top * h) - y;
                break;
            }
    }
    // Crop the viewport around its camera center so wide or vertical splits
    // keep the same proportions on a 16:9 card.
    const int crop_width = std::min(width, height * kWidth / kHeight);
    const int crop_height = std::min(height, width * kHeight / kWidth);
    x += (width - crop_width) / 2;
    y += (height - crop_height) / 2;
    width = crop_width;
    height = crop_height;
    if (width <= 0 || height <= 0) return;
    std::vector<unsigned char> pixels(size_t(width) * height * 4), rgb(kWidth * kHeight * 3);
    g->PushRenderTarget();
    g->BindWindow();
    g->ReadPixels(PixelRect(x, y, width, height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g->PopRenderTarget();
    for (int row = 0; row < kHeight; ++row)
        for (int col = 0; col < kWidth; ++col) {
            const size_t from = (size_t(height - 1 - row * height / kHeight) * width + col * width / kWidth) * 4;
            std::copy_n(pixels.data() + from, 3, rgb.data() + (row * kWidth + col) * 3);
        }
    Json::Value record;
    record["version"] = 1;
    record["date"] = Now();
    record["map"] = CurrentMap();
    auto* campaign = engine->GetCurrentCampaign();
    const std::string camp = campaign ? campaign->GetCampaignID() : net->LobbyCampaign();
    record["campaign"] = camp;
    record["mode"] = net->IsActive() ? net->LobbyMode() : camp;
    record["lobby"] = net->IsHosting() && (!net->IsLocalOnlySession() || net->GetLobbySlots().size() > 1);
    record["local_only"] = net->IsLocalOnlySession();
    record["map_name"] = graph->level_name_.empty() ? SplitPathFileName(CurrentMap()).second : graph->level_name_;
    record["mode_name"] = camp.empty() ? (net->LobbyBuild() ? "Build" : "Sandbox") : camp;
    if (!camp.empty()) {
        auto lookup = camp;
        auto metadata = ModLoading::Instance().GetCampaign(lookup);
        if (!metadata.title.str().empty()) record["mode_name"] = metadata.title.str();
        for (const auto& entry : metadata.levels)
            if (MapPath("Data/Levels/" + entry.path.str()) == CurrentMap() && !entry.title.str().empty())
                record["map_name"] = entry.title.str();
    }
    record["rules"] = net->SaveGameRules();
    if (record["lobby"].asBool()) record["slots"] = net->SaveLobbySlots();
    record["progression"] = FriendlyProgression::Snapshot();
    record["world"] = CaptureWorld(graph);
    for (int slot : requested) {
        const bool ok = Commit(slot, record, rgb);
        if (slot || !ok) status = ok ? "Saved slot " + std::to_string(slot) + "." : "Could not write the save to disk.";
    }
    requested.clear();
}
int ArenaRound(int round) {
    if (stage == Stage::LoadWorld && arena_restore["arena"].isNull() && arena_restore.get("round", 0).asInt() > 0) round = arena_restore["round"].asInt();
    arena_round = round;
    arena_points = Json::Value(Json::objectValue);
    return round;
}
int ArenaPoints(int avatar, int points) {
    auto* graph = Engine::Instance()->GetSceneGraph();
    Object* obj = graph ? graph->GetObjectFromID(avatar) : nullptr;
    if (!obj || obj->GetType() != _movement_object) return points;
    const auto key = ActorKey(static_cast<MovementObject*>(obj));
    if (!key.empty()) {
        if (stage == Stage::LoadWorld && arena_restore["arena"].isNull() && arena_restore["points"][key].isInt()) points = arena_restore["points"][key].asInt();
        arena_points[key] = points;
    }
    return points;
}
void ArenaState(const std::string& state) {
    if (Online::Instance()->IsClient() || state.size() > kMaxBytes) return;
    Json::Value value;
    if (!Json::Reader().parse(state, value, false) || !ValidArenaState(value)) return;
    arena_state = std::move(value);
    arena_round = arena_state["round"].asInt();
    arena_points = Json::Value(Json::objectValue);
    for (const auto& fighter : arena_state["fighters"])
        arena_points[fighter["key"].asString()] = fighter["points"];
}
std::string ArenaRestore() {
    // Progression includes the remaining shop time. Let native restoration
    // apply that ledger before the Arena decides whether to deploy or shop.
    return stage == Stage::LoadWorld && ledger_applied && arena_restore["arena"].isObject()
               ? Json::FastWriter().write(arena_restore["arena"]) : "";
}
}  // namespace SessionSaves
