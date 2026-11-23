// Host-owned progression and mode-specific lobby preferences. Apache-2.0.
#include <Online/online.h>
#include <Online/challenge_scaling.h>
#include <Online/online_utility.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Game/level.h>
#include <Editors/map_editor.h>
#include <Objects/movementobject.h>
#include <Objects/hotspot.h>
#include <JSON/json.h>
#include <algorithm>
#include <cmath>

struct ChallengeRuntimeState {
    ChallengeScaling::State progress;
    SceneGraph* graph = nullptr;
    std::string level, encounter;
    std::map<PlayerID, std::pair<ObjectID, int>> health;
    std::set<ObjectID> foes;
    uint64_t death_serial = 0, combat_serial = 0;
    double last_tick = 0, last_publish = 0;
};
namespace {
const char* const keys[] = {"challenge_time", "challenge_round", "challenge_win", "challenge_death", "challenge_adaptive"};
bool Known(const std::string& key) { for (const char* candidate : keys) if (key == candidate) return true; return false; }
bool CampaignMode(const Online* online) {
    return !online->LobbyCampaign().empty() && online->LobbyMode() != "com-wolfire-arena" &&
        online->LobbyMode() != "sandbox" && online->LobbyCampaign() != "com-wolfire-sandbox-levels" && !online->LobbyBuild();
}
MovementObject* ChallengeActor(SceneGraph* graph, ObjectID id) {
    Object* object = graph ? graph->GetObjectFromID(id) : nullptr;
    return object && object->GetType() == _movement_object ? static_cast<MovementObject*>(object) : nullptr;
}
bool Awake(MovementObject* character) {
    return character && character->ASHasVar("knocked_out") && character->ASGetIntVar("knocked_out") == 0;
}
ChallengeScaling::Settings Settings(const Online* online) {
    ChallengeScaling::Settings settings;
    settings.time_rate = online->ModeSetting("challenge_time");
    settings.round_rate = online->ModeSetting("challenge_round");
    settings.win_rate = online->ModeSetting("challenge_win");
    settings.death_rate = online->ModeSetting("challenge_death");
    settings.adaptive = online->ModeSetting("challenge_adaptive") != 0 && settings.win_rate != 0;
    return settings;
}
bool Playable(Online* online) {
    Engine* engine = Engine::Instance();
    SceneGraph* graph = engine->GetSceneGraph();
    return online->IsHosting() && !online->LobbyWaiting() && !online->LobbyBuild() && !online->BuildSession() &&
        online->host_started_level && engine->IsLevelLoaded() && graph && graph->level && graph->map_editor &&
        graph->map_editor->state_ == MapEditor::kInGame;
}
std::string Key(Online* online, const std::string& key) { return online->level_name + "|" + key; }
void SampleDeaths(Online* online, ChallengeRuntimeState& state) {
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    MovementObject* reference = ChallengeActor(graph, online->ChallengeReferenceAvatar());
    if (!reference) return;
    std::set<PlayerID> present;
    for (const auto& player : online->GetPlayerStates()) {
        if (player.is_bot) continue;  // party bots contribute size; defeat progression counts humans
        MovementObject* actor = ChallengeActor(graph, player.object_id);
        if (!actor || !actor->enabled_ || !actor->ASOnSameTeam(reference) || !actor->ASHasVar("knocked_out")) continue;
        present.insert(player.player_id);
        const int health = actor->ASGetIntVar("knocked_out");
        auto previous = state.health.find(player.player_id);
        if (previous != state.health.end() && previous->second.first == actor->GetID() && previous->second.second == 0 && health != 0)
            state.progress.Death("death:" + std::to_string(++state.death_serial), state.encounter, Settings(online));
        state.health[player.player_id] = {actor->GetID(), health};
    }
    for (auto it = state.health.begin(); it != state.health.end();)
        if (!present.count(it->first)) it = state.health.erase(it); else ++it;
}
bool Attacking(MovementObject* actor, ObjectID target) {
    if (actor->ASHasVar("state") && actor->ASGetIntVar("state") == 2 && actor->ASHasVar("target_id") && actor->ASGetIntVar("target_id") == target) return true;
    return actor->ASHasVar("goal") && actor->ASGetIntVar("goal") == 1 && actor->ASHasVar("chase_target_id") &&
        actor->ASGetIntVar("chase_target_id") == target &&
        (!actor->ASHasVar("combat_allowed") || actor->ASGetBoolVar("combat_allowed"));
}
}
std::string Online::ModeSettingsKey() const { return lobby_build_ ? "build" : lobby_mode_; }
int Online::ModeSetting(const std::string& key) const {
    const auto mode = mode_settings_.find(ModeSettingsKey());
    if (mode == mode_settings_.end()) return 0;
    const auto setting = mode->second.find(key);
    return setting == mode->second.end() ? 0 : setting->second;
}
bool Online::SetModeSetting(const std::string& key, int value, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!IsActive() || !lobby_waiting_ || LobbyMigrationBusy() || !IsLobbyHost(actor) || lobby_build_ || lobby_mode_.empty() ||
        !Known(key) || value < 0 || value > (key == "challenge_adaptive" ? 1 : 3)) return false;
    if (key == "challenge_round" && !CampaignMode(this) && lobby_mode_ != "com-wolfire-arena") return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "mode_setting"; request["actor"] = actor;
        request["key"] = key; request["value"] = value;
        SubmitLobbyRequest(Json::FastWriter().write(request)); return true;
    }
    auto& settings = mode_settings_[ModeSettingsKey()];
    if (key == "challenge_adaptive" && value && settings["challenge_win"] == 0) return false;
    settings[key] = value;
    if (key == "challenge_win" && value == 0) settings["challenge_adaptive"] = 0;
    BroadcastLobby(); return true;
}
void Online::ResetModeChallenge() { mode_challenge_state = std::make_shared<ChallengeRuntimeState>(); }
void Online::ChallengeSceneChanged() {
    if (!mode_challenge_state) return;
    mode_challenge_state->graph = nullptr;
    mode_challenge_state->last_tick = 0;
    mode_challenge_state->health.clear();
    mode_challenge_state->foes.clear();
}
ObjectID Online::ChallengeReferenceAvatar() const {
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    const ObjectID main = CampaignMainAvatarID();
    if (ChallengeActor(graph, main)) return main;
    if (ChallengeActor(graph, HostAvatarID())) return HostAvatarID();
    if (online_session) for (const auto& entry : online_session->player_states)
        if (!entry.second.is_bot && ChallengeActor(graph, entry.second.object_id)) return entry.second.object_id;
    return static_cast<ObjectID>(-1);
}
float Online::ModeChallengePressure() const { return mode_challenge_state ? static_cast<float>(mode_challenge_state->progress.Data().pressure) : 0.0f; }
void Online::ChallengeEncounterBegin(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    // A campaign map may also be hosted as sandbox. Its legacy level script
    // must not replace sandbox combat groups with a campaign-wide objective.
    if (key.compare(0, 9, "campaign:") == 0 && !CampaignMode(this)) return;
    if (!mode_challenge_state) ResetModeChallenge();
    mode_challenge_state->graph = Engine::Instance()->GetSceneGraph();
    mode_challenge_state->level = level_name;
    SampleDeaths(this, *mode_challenge_state);
    const std::string incoming = Key(this, key);
    if (mode_challenge_state->encounter != incoming) mode_challenge_state->foes.clear();
    mode_challenge_state->encounter = incoming;
}
void Online::ChallengeEncounterWin(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    if (key.compare(0, 9, "campaign:") == 0 && !CampaignMode(this)) return;
    if (!mode_challenge_state) ResetModeChallenge();
    SampleDeaths(this, *mode_challenge_state);
    if (mode_challenge_state->progress.Win(Key(this, key), Settings(this))) BroadcastLobby();
}
void Online::ChallengeCompletion(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    if (!CampaignMode(this) && lobby_mode_ != "com-wolfire-arena") return;
    if (!mode_challenge_state) ResetModeChallenge();
    SampleDeaths(this, *mode_challenge_state);
    if (mode_challenge_state->progress.Completion(Key(this, key), Settings(this))) BroadcastLobby();
}
void Online::UpdateModeChallenge(SceneGraph* graph) {
    if (!IsHosting()) return;
    if (!mode_challenge_state) ResetModeChallenge();
    auto& state = *mode_challenge_state;
    const double now = OnlineClockMs();
    const double elapsed = state.last_tick ? std::max(0.0, std::min(1.0, (now - state.last_tick) / 1000.0)) : 0;
    state.last_tick = now;
    if (!Playable(this)) return;
    const auto settings = Settings(this);
    if (settings.time_rate == 0 && settings.round_rate == 0 && settings.win_rate == 0 && settings.death_rate == 0) return;
    if (state.graph != graph || state.level != level_name) {
        if (state.level != level_name) state.encounter.clear();
        state.graph = graph; state.level = level_name; state.health.clear(); state.foes.clear();
        state.last_tick = now;
        if (settings.death_rate || settings.adaptive) SampleDeaths(this, state);
        return;
    }
    Engine* engine = Engine::Instance();
    if (TimeFrozen() || engine->paused || engine->waiting_for_input_ || CoopCutscene() || graph->level->DialogueCameraControl()) return;
    state.progress.Advance(elapsed, settings);
    if (settings.death_rate || settings.adaptive) SampleDeaths(this, state);
    // Modes without authored round/goal events use the current combat group.
    bool authored = false;
    if (CampaignMode(this)) for (Object* object : graph->hotspots_)
        if (static_cast<Hotspot*>(object)->GetScriptFile().find("overgrowth_level") != std::string::npos) { authored = true; break; }
    const auto encounter = state.progress.Data().encounters.find(state.encounter);
    const bool explicit_encounter = !state.encounter.empty() && state.encounter.find("|combat:") == std::string::npos &&
        (encounter == state.progress.Data().encounters.end() || !encounter->second.won);
    if (settings.win_rate && !authored && !explicit_encounter) {
        MovementObject* reference = ChallengeActor(graph, ChallengeReferenceAvatar());
        bool survivor = false;
        for (const auto& player : GetPlayerStates()) {
            MovementObject* member = ChallengeActor(graph, player.object_id);
            if (!reference || !Awake(member) || !member->enabled_ || !member->ASOnSameTeam(reference)) continue;
            if (!player.is_bot) survivor = true;
            for (Object* object : graph->movement_objects_) {
                auto* foe = static_cast<MovementObject*>(object);
                if (!foe->enabled_ || !Awake(foe) || foe->ASOnSameTeam(member) || distance_squared(foe->position, member->position) > 1225.0f) continue;
                if (Attacking(member, foe->GetID()) || Attacking(foe, member->GetID())) {
                    if (state.foes.empty()) {
                        const auto previous = state.progress.Data().encounters.find(state.encounter);
                        const bool unfinished = state.encounter.find(level_name + "|combat:") == 0 &&
                            (previous == state.progress.Data().encounters.end() || !previous->second.won);
                        if (!unfinished) state.encounter = Key(this, "combat:" + std::to_string(++state.combat_serial));
                    }
                    state.foes.insert(foe->GetID());
                }
            }
        }
        if (!state.foes.empty() && survivor) {
            bool defeated = true;
            for (ObjectID id : state.foes) if (Awake(ChallengeActor(graph, id))) { defeated = false; break; }
            if (defeated) { state.progress.Win(state.encounter, Settings(this)); state.foes.clear(); }
        }
    }
    if (now - state.last_publish > 5000.0) { state.last_publish = now; BroadcastLobby(); }
}
std::string Online::ModeChallengePayload() const {
    Json::Value root;
    root["modes"] = Json::Value(Json::objectValue);
    for (const auto& mode : mode_settings_) for (const auto& setting : mode.second) root["modes"][mode.first][setting.first] = setting.second;
    if (mode_challenge_state) {
        const auto& state = *mode_challenge_state;
        const auto& data = state.progress.Data();
        root["seconds"] = data.active_seconds; root["pressure"] = data.pressure;
        root["death_serial"] = Json::UInt64(state.death_serial); root["combat_serial"] = Json::UInt64(state.combat_serial);
        root["encounter"] = state.encounter;
        root["level"] = state.level;
        root["foes"] = Json::Value(Json::arrayValue);
        for (ObjectID id : state.foes) root["foes"].append(static_cast<int>(GetOriginalID(id)));
        root["completions"] = Json::Value(Json::arrayValue); root["deaths"] = Json::Value(Json::arrayValue);
        for (const auto& id : data.completions) root["completions"].append(id);
        for (const auto& id : data.deaths) root["deaths"].append(id);
        root["encounters"] = Json::Value(Json::objectValue);
        for (const auto& entry : data.encounters) {
            auto& record = root["encounters"][entry.first]; record["deaths"] = entry.second.deaths;
            record["won"] = entry.second.won; record["award"] = entry.second.win_award;
        }
    }
    return Json::FastWriter().write(root);
}
void Online::ReceiveModeChallenge(const std::string& payload) {
    if (payload.size() > 512 * 1024) return;
    Json::Reader reader; Json::Value root;
    if (!reader.parse(payload, root, false) || !root.isObject()) return;
    if (root["modes"].isObject() && root["modes"].size() <= 128) {
        std::map<std::string, std::map<std::string, int>> incoming;
        for (const auto& mode : root["modes"].getMemberNames()) {
            if (mode.size() > 256 || !root["modes"][mode].isObject()) continue;
            for (const char* key : keys) {
                const auto& value = root["modes"][mode][key];
                if (value.isInt() && value.asInt() >= 0 && value.asInt() <= (std::string(key) == "challenge_adaptive" ? 1 : 3)) incoming[mode][key] = value.asInt();
            }
        }
        mode_settings_ = incoming;
    }
    if (!root["pressure"].isNumeric() || !root["seconds"].isNumeric() || !root["completions"].isArray() ||
        !root["deaths"].isArray() || !root["encounters"].isObject() || !root["encounter"].isString() || !root["level"].isString()) return;
    ChallengeScaling::Snapshot data;
    data.pressure = root["pressure"].asDouble(); data.active_seconds = root["seconds"].asDouble();
    for (const auto& id : root["completions"]) if (id.isString() && id.asString().size() <= 8192) data.completions.insert(id.asString());
    for (const auto& id : root["deaths"]) if (id.isString() && id.asString().size() <= 8192) data.deaths.insert(id.asString());
    if (root["encounters"].isObject()) for (const auto& id : root["encounters"].getMemberNames()) {
        const auto& row = root["encounters"][id];
        if (id.size() > 8192 || !row["deaths"].isUInt() || !row["won"].isBool() || !row["award"].isNumeric()) continue;
        auto& record = data.encounters[id]; record.deaths = row["deaths"].asUInt(); record.won = row["won"].asBool(); record.win_award = row["award"].asDouble();
    }
    if (!mode_challenge_state) ResetModeChallenge();
    if (!mode_challenge_state->progress.Restore(data)) return;
    mode_challenge_state->encounter = root.get("encounter", "").asString();
    mode_challenge_state->level = root.get("level", "").asString();
    if (root["death_serial"].isUInt64()) mode_challenge_state->death_serial = root["death_serial"].asUInt64();
    if (root["combat_serial"].isUInt64()) mode_challenge_state->combat_serial = root["combat_serial"].asUInt64();
}
