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
#include <Internal/config.h>
#include <algorithm>
#include <cmath>

struct ChallengeRuntimeState {
    ChallengeScaling::State progress;
    SceneGraph* graph = nullptr;
    std::string level, encounter;
    std::set<ObjectID> foes;
    std::set<CommonObjectID> restored_foes;
    uint64_t wipe_serial = 0, combat_serial = 0;
    ChallengeScaling::WipeTracker wipe_tracker;
    double last_tick = 0, last_publish = 0;
};
namespace {
const char* const keys[] = {"challenge_time", "challenge_round", "challenge_performance", "challenge_performance_rate"};
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
    if (!online->DifficultyScalingEnabled()) return settings;
    settings.time_rate = online->ModeSetting("challenge_time");
    settings.round_rate = online->ModeSetting("challenge_round");
    settings.performance_mode = online->ModeSetting("challenge_performance");
    settings.performance_rate = online->ModeSetting("challenge_performance_rate");
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
bool SampleWipes(Online* online, ChallengeRuntimeState& state) {
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    MovementObject* reference = ChallengeActor(graph, online->ChallengeReferenceAvatar());
    if (!reference) return false;
    size_t humans = 0, awake = 0;
    for (const auto& player : online->GetPlayerStates()) {
        if (player.is_bot) continue;
        MovementObject* actor = ChallengeActor(graph, player.object_id);
        if (!actor || !actor->ASHasVar("knocked_out")) return false;
        if (!actor->enabled_ || !actor->ASOnSameTeam(reference)) continue;
        ++humans;
        if (Awake(actor)) ++awake;
    }
    if (!state.wipe_tracker.Observe(static_cast<uint32_t>(humans), static_cast<uint32_t>(awake), OnlineClockMs())) return false;
    if (state.encounter.empty()) state.encounter = Key(online, "retry:level");
    const bool changed = state.progress.Wipe("wipe:" + std::to_string(++state.wipe_serial), state.encounter, Settings(online));
    if (changed) online->BroadcastLobby();
    return changed;
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
    if (mode == mode_settings_.end()) return key == "challenge_performance_rate" ? ChallengeScaling::Standard : 0;
    const auto setting = mode->second.find(key);
    return setting == mode->second.end() ? (key == "challenge_performance_rate" ? ChallengeScaling::Standard : 0) : setting->second;
}
bool Online::SetModeSetting(const std::string& key, int value, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!IsActive() || LobbyMigrationBusy() || !IsLobbyHost(actor) || lobby_build_ || lobby_mode_.empty() ||
        !Known(key) || value < 0 || value > 3) return false;
    if (key == "challenge_round" && !CampaignMode(this) && lobby_mode_ != "com-wolfire-arena") return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "mode_setting"; request["actor"] = actor;
        request["key"] = key; request["value"] = value;
        SubmitLobbyRequest(Json::FastWriter().write(request)); return true;
    }
    // Close any just-finished attempt under its existing rules before a live
    // edit. Enabling a driver cannot retroactively price an earlier defeat.
    if (Playable(this)) {
        if (!mode_challenge_state) ResetModeChallenge();
        SampleWipes(this, *mode_challenge_state);
    }
    auto& settings = mode_settings_[ModeSettingsKey()];
    settings[key] = value;
    BroadcastLobby(); return true;
}
void Online::ResetModeChallenge() { mode_challenge_state = std::make_shared<ChallengeRuntimeState>(); }
void Online::ChallengeSceneChanged() {
    if (!mode_challenge_state) return;
    mode_challenge_state->graph = nullptr;
    mode_challenge_state->last_tick = 0;
    mode_challenge_state->wipe_tracker.all_awake_since = 0;
    mode_challenge_state->foes.clear();
    mode_challenge_state->restored_foes.clear();
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
bool Online::DifficultyScalingEnabled() const { return config["difficulty_scaling"].toBool(); }
float Online::ModeChallengeGeneralPressure() const {
    return DifficultyScalingEnabled() && mode_challenge_state ? static_cast<float>(mode_challenge_state->progress.Data().pressure) : 0.0f;
}
float Online::ModeChallengePressure() const {
    if (!DifficultyScalingEnabled() || !mode_challenge_state) return 0.0f;
    const auto& state = *mode_challenge_state;
    return static_cast<float>(ChallengeScaling::EffectivePressure(state.progress.Data(), Settings(this), state.level == level_name ? state.encounter : ""));
}
void Online::DescribeChallengeHUD(Json::Value& result) const {
    result = Json::Value(Json::objectValue);
    const ChallengeScaling::Snapshot empty;
    const bool session = IsActive();
    const auto settings = session ? Settings(this) : ChallengeScaling::Settings();
    const auto& data = session && mode_challenge_state ? mode_challenge_state->progress.Data() : empty;
    const std::string encounter = session && mode_challenge_state && mode_challenge_state->level == level_name ? mode_challenge_state->encounter : "";
    const auto preview = ChallengeScaling::PreviewWin(data, settings, encounter);
    unsigned wins = 0;
    for (const auto& record : data.encounters) if (record.second.won) ++wins;
    result["master_enabled"] = DifficultyScalingEnabled();
    result["pressure"] = DifficultyScalingEnabled() ? preview.pressure : 0; result["next_pressure"] = DifficultyScalingEnabled() ? preview.next_pressure : 0;
    result["general_pressure"] = preview.general_pressure; result["encounter_relief"] = preview.encounter_relief;
    result["active_seconds"] = data.active_seconds;
    result["wins"] = wins; result["completions"] = Json::UInt64(data.completions.size()); result["wipes"] = Json::UInt64(data.wipes.size());
    result["encounter"] = encounter; result["encounter_wipes"] = preview.encounter_wipes;
    result["win_pending"] = preview.pending;
    result["performance"]["mode"] = settings.performance_mode; result["performance"]["rate"] = settings.performance_rate;
    result["expected_win_award"] = preview.award;
    result["time_per_second"] = preview.time_per_second;
    result["completion_award"] = preview.completion_award; result["wipe_award"] = preview.wipe_award;
    result["rates"]["time"] = settings.time_rate; result["rates"]["completion"] = settings.round_rate;
    result["rates"]["performance"] = settings.performance_rate;
    result["completion_supported"] = session && (CampaignMode(this) || lobby_mode_ == "com-wolfire-arena");
    result["preview_basis"] = preview.pending ? "After the current win; no extra time, party defeat or level/match award" : "Same pressure; no unfinished encounter award";
    Engine* engine = Engine::Instance();
    SceneGraph* graph = engine->GetSceneGraph();
    const bool frozen = online_session && TimeFrozen();
    const bool paused = engine->paused || frozen || engine->waiting_for_input_;
    const bool cutscene = CoopCutscene() || (graph && graph->level && graph->level->DialogueCameraControl());
    const bool enabled = settings.time_rate || settings.round_rate || (settings.performance_mode && settings.performance_rate);
    result["clock_active"] = session && enabled && !paused && !cutscene && !loading && !LobbyWaiting() && !LobbyBuild() && !BuildSession();
    result["time_active"] = result["clock_active"].asBool() && settings.time_rate != 0;
    result["clock_state"] = !session || !enabled ? "off" : loading || LobbyWaiting() ? "waiting" : cutscene ? "cutscene" : paused ? "paused" : "active";
}
void Online::ChallengeEncounterBegin(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    // A campaign map may also be hosted as sandbox. Its legacy level script
    // must not replace sandbox combat groups with a campaign-wide objective.
    if (key.compare(0, 9, "campaign:") == 0 && !CampaignMode(this)) return;
    if (!mode_challenge_state) ResetModeChallenge();
    mode_challenge_state->graph = Engine::Instance()->GetSceneGraph();
    mode_challenge_state->level = level_name;
    SampleWipes(this, *mode_challenge_state);
    const std::string incoming = Key(this, key);
    if (mode_challenge_state->encounter != incoming) {
        mode_challenge_state->foes.clear();
        mode_challenge_state->restored_foes.clear();
    }
    mode_challenge_state->encounter = incoming;
}
void Online::ChallengeEncounterWin(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    if (key.compare(0, 9, "campaign:") == 0 && !CampaignMode(this)) return;
    if (!mode_challenge_state) ResetModeChallenge();
    SampleWipes(this, *mode_challenge_state);
    if (mode_challenge_state->progress.Win(Key(this, key), Settings(this))) BroadcastLobby();
}
void Online::ChallengeCompletion(const std::string& key) {
    if (!Playable(this) || key.empty() || key.size() > 512) return;
    if (!CampaignMode(this) && lobby_mode_ != "com-wolfire-arena") return;
    if (!mode_challenge_state) ResetModeChallenge();
    SampleWipes(this, *mode_challenge_state);
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
    if (state.graph != graph || state.level != level_name) {
        if (state.level != level_name) { state.encounter.clear(); state.restored_foes.clear(); }
        state.graph = graph; state.level = level_name; state.wipe_tracker.all_awake_since = 0; state.foes.clear();
        state.last_tick = now;
        SampleWipes(this, state);
        return;
    }
    Engine* engine = Engine::Instance();
    if (TimeFrozen() || engine->paused || engine->waiting_for_input_ || CoopCutscene() || graph->level->DialogueCameraControl()) return;
    SampleWipes(this, state);
    if (settings.time_rate == 0 && settings.round_rate == 0 && (settings.performance_mode == 0 || settings.performance_rate == 0)) return;
    for (auto it = state.restored_foes.begin(); it != state.restored_foes.end();) {
        const ObjectID id = GetObjectID(*it);
        if (ChallengeActor(graph, id)) { state.foes.insert(id); it = state.restored_foes.erase(it); }
        else ++it;
    }
    state.progress.Advance(elapsed, settings);
    // Modes without authored round/goal events use the current combat group.
    bool authored = false;
    if (CampaignMode(this)) for (Object* object : graph->hotspots_)
        if (static_cast<Hotspot*>(object)->GetScriptFile().find("overgrowth_level") != std::string::npos) { authored = true; break; }
    const auto encounter = state.progress.Data().encounters.find(state.encounter);
    const bool explicit_encounter = !state.encounter.empty() && state.encounter.find("|combat:") == std::string::npos &&
        (encounter == state.progress.Data().encounters.end() || !encounter->second.won);
    if (settings.performance_mode && settings.performance_rate && !authored && !explicit_encounter) {
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
        if (!state.foes.empty() && state.restored_foes.empty() && survivor) {
            bool defeated = true;
            for (ObjectID id : state.foes) if (Awake(ChallengeActor(graph, id))) { defeated = false; break; }
            if (defeated) { state.progress.Win(state.encounter, Settings(this)); state.foes.clear(); }
        }
    }
    if (now - state.last_publish > 5000.0) { state.last_publish = now; BroadcastLobby(); }
}
std::string Online::ModeChallengePayload() const {
    Json::Value root;
    root["version"] = 2;
    root["modes"] = Json::Value(Json::objectValue);
    for (const auto& mode : mode_settings_) for (const auto& setting : mode.second) root["modes"][mode.first][setting.first] = setting.second;
    if (mode_challenge_state) {
        const auto& state = *mode_challenge_state;
        const auto& data = state.progress.Data();
        root["seconds"] = data.active_seconds; root["pressure"] = data.pressure;
        root["wipe_serial"] = Json::UInt64(state.wipe_serial); root["combat_serial"] = Json::UInt64(state.combat_serial);
        root["wipe_latched"] = state.wipe_tracker.latched;
        root["encounter"] = state.encounter; root["level"] = state.level;
        root["foes"] = Json::Value(Json::arrayValue);
        for (ObjectID id : state.foes) root["foes"].append(static_cast<int>(GetOriginalID(id)));
        for (CommonObjectID id : state.restored_foes) root["foes"].append(static_cast<int>(id));
        root["completions"] = Json::Value(Json::arrayValue); root["wipes"] = Json::Value(Json::arrayValue);
        for (const auto& id : data.completions) root["completions"].append(id);
        for (const auto& id : data.wipes) root["wipes"].append(id);
        root["encounters"] = Json::Value(Json::objectValue);
        for (const auto& entry : data.encounters) {
            auto& record = root["encounters"][entry.first]; record["wipes"] = entry.second.wipes;
            record["relief"] = entry.second.relief;
            record["won"] = entry.second.won; record["award"] = entry.second.win_award;
        }
    }
    return Json::FastWriter().write(root);
}
void Online::ReceiveModeChallenge(const std::string& payload) {
    if (payload.size() > 512 * 1024) return;
    Json::Reader reader; Json::Value root;
    if (!reader.parse(payload, root, false) || !root.isObject() || !root["version"].isInt() || root["version"].asInt() != 2) return;
    std::map<std::string, std::map<std::string, int>> incoming;
    if (!root["modes"].isObject() || root["modes"].size() > 128) return;
    for (const auto& mode : root["modes"].getMemberNames()) {
        if (mode.size() > 256 || !root["modes"][mode].isObject()) return;
        for (const char* key : keys) {
            const auto& value = root["modes"][mode][key];
            if (value.isNull()) continue;
            if (!value.isInt() || value.asInt() < 0 || value.asInt() > 3) return;
            incoming[mode][key] = value.asInt();
        }
    }
    // A lobby with no started challenge still carries per-mode preferences.
    if (root["pressure"].isNull()) { mode_settings_ = incoming; return; }
    if (!root["pressure"].isNumeric() || !root["seconds"].isNumeric() || !root["completions"].isArray() ||
        !root["wipes"].isArray() || !root["encounters"].isObject() || !root["encounter"].isString() || !root["level"].isString() ||
        !root["wipe_latched"].isBool() || !root["wipe_serial"].isUInt64() || !root["combat_serial"].isUInt64() ||
        !root["foes"].isArray() || root["foes"].size() > 8192 || root["encounter"].asString().size() > 1024 || root["level"].asString().size() > 1024) return;
    ChallengeScaling::Snapshot data;
    data.pressure = root["pressure"].asDouble(); data.active_seconds = root["seconds"].asDouble();
    for (const auto& id : root["completions"]) { if (!id.isString()) return; data.completions.insert(id.asString()); }
    for (const auto& id : root["wipes"]) { if (!id.isString()) return; data.wipes.insert(id.asString()); }
    for (const auto& id : root["encounters"].getMemberNames()) {
        const auto& row = root["encounters"][id];
        if (!row["wipes"].isUInt() || !row["won"].isBool() || !row["award"].isNumeric() || !row["relief"].isNumeric()) return;
        auto& record = data.encounters[id]; record.wipes = row["wipes"].asUInt(); record.won = row["won"].asBool();
        record.win_award = row["award"].asDouble(); record.relief = row["relief"].asDouble();
    }
    ChallengeScaling::State validated;
    if (!validated.Restore(data) || root["wipe_serial"].asUInt64() < data.wipes.size()) return;
    if (!mode_challenge_state) ResetModeChallenge();
    auto& state = *mode_challenge_state;
    state.progress = validated; mode_settings_ = incoming;
    state.encounter = root["encounter"].asString(); state.level = root["level"].asString();
    state.wipe_serial = root["wipe_serial"].asUInt64(); state.combat_serial = root["combat_serial"].asUInt64();
    state.wipe_tracker.latched = root["wipe_latched"].asBool(); state.wipe_tracker.all_awake_since = 0;
    state.foes.clear(); state.restored_foes.clear();
    for (const auto& id : root["foes"]) if (id.isInt() && id.asInt() >= 0) state.restored_foes.insert(static_cast<CommonObjectID>(id.asInt()));
}
