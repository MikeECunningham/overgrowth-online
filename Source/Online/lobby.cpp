// Session roster and pre-game lobby control. Apache-2.0.
#include <Online/online.h>
#include <Online/session_saves.h>
#include <Online/session_mods.h>
#include <Online/friendly_progression.h>
#include <Online/online_utility.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <JSON/json.h>
#include <Internal/config.h>
#include <Internal/settings_changes.h>
#include <Graphics/camera.h>
#include <Editors/map_editor.h>
#include <Internal/modloading.h>
#include <Game/level.h>
#include <Scripting/angelscript/lobby_bindings.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>

namespace {
std::string Encode(const Json::Value& value) { return Json::FastWriter().write(value); }
std::string NameOrDefault(const std::string& name, unsigned index) {
    if (OnlineUtility::IsValidPlayerName(name)) return name;
    return index == 0 ? "Unknown Rabbit" : "Local player " + std::to_string(index + 1);
}
std::string NewSlotUID(PlayerID id) {
    static uint64_t serial = 0;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::to_string(stamp) + "-" + std::to_string(++serial) + "-" + std::to_string(id);
}
int TeamChoiceLimit(const std::map<PlayerID, LobbySlot>& slots) {
    int count = static_cast<int>(slots.size());
    for (const auto& entry : slots) count = std::max(count, entry.second.team);
    return std::min(4096, count);
}
bool DecodeSlotDefinitions(const Json::Value& saved, std::map<PlayerID, LobbySlot>& slots, PlayerID& next) {
    if (!saved.isObject() || !saved["version"].isInt() || saved["version"].asInt() != 1 ||
        !saved["auto_admit"].isBool() || !saved["host_slot"].isUInt() || !saved["next_slot_id"].isUInt() ||
        !saved["slots"].isArray() || saved["slots"].size() == 0 || saved["slots"].size() > LobbyAuthority::kMaxSlots) return false;
    std::set<std::string> identities;
    std::set<uint32_t> orders;
    next = saved["next_slot_id"].asUInt();
    if (next < 256 || next == LobbyAuthority::InvalidPlayer) return false;
    for (const auto& row : saved["slots"]) {
        if (!row.isObject() || !row["id"].isUInt() || row["id"].asUInt() >= LobbyAuthority::InvalidPlayer - 1 ||
            !row["order"].isUInt() || row["order"].asUInt() == 0 || row["order"].asUInt() == UINT32_MAX ||
            !row["uid"].isString() || !row["kind"].isString() || !row["name"].isString() || !row["team"].isInt()) return false;
        LobbySlot slot;
        slot.id = row["id"].asUInt(); slot.order = row["order"].asUInt(); slot.uid = row["uid"].asString(); slot.name = row["name"].asString(); slot.team = row["team"].asInt();
        const auto kind = row["kind"].asString();
        // Removing a bot during play retains the surviving teams until the
        // lobby reopens, so their labels may exceed the reduced slot count.
        if ((kind != "player" && kind != "bot") || slot.uid.empty() || slot.uid.size() > 96 ||
            !std::all_of(slot.uid.begin(), slot.uid.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; }) ||
            !OnlineUtility::IsValidPlayerName(slot.name) || slot.team < 1 || slot.team > 4096 ||
            slots.count(slot.id) || !identities.insert(slot.uid).second || !orders.insert(slot.order).second) return false;
        slot.is_bot = kind == "bot";
        slots[slot.id] = slot;
        if (slot.id >= next) next = slot.id + 1;
    }
    const PlayerID host = saved["host_slot"].asUInt();
    if (!slots.count(host) || slots.at(host).is_bot || next == LobbyAuthority::InvalidPlayer) return false;
    return true;
}
std::map<PlayerID, ObjectID> initialized_appearances;
const SceneGraph* appearance_graph = nullptr;
std::string appearance_level;
std::string natural_player_team;
std::string natural_team_level;
const SceneGraph* natural_team_graph = nullptr;

void CaptureNaturalPlayerTeam(SceneGraph* graph, const std::string& level, ObjectID avatar) {
    if (natural_team_graph != graph || natural_team_level != level) {
        natural_player_team.clear();
        natural_team_graph = graph;
        natural_team_level = level;
    }
    if (!natural_player_team.empty()) return;
    Object* object = graph->GetObjectFromID(avatar);
    if (object && object->GetScriptParams()->HasParam("Teams"))
        natural_player_team = object->GetScriptParams()->GetStringVal("Teams");
}

const char* const migration_match_settings[] = {
    "oo_arena_bots", "oo_arena_random", "oo_public_game", "game_difficulty",
    "global_time_scale_mult", "tutorials", "auto_ledge_grab", "campaign_mp_scaling", "difficulty_scaling", "campaign_ai_scaling", "campaign_ai_scaling_rate", "campaign_armor_scaling", "campaign_weapon_scaling", "armor_localized"
};
const char* const campaign_match_settings[] = {
    "game_difficulty", "global_time_scale_mult", "tutorials", "auto_ledge_grab",
    "campaign_mp_scaling", "difficulty_scaling", "campaign_ai_scaling", "campaign_ai_scaling_rate", "campaign_armor_scaling", "campaign_weapon_scaling", "armor_localized", "oo_arena_random"
};

bool MatchSettingAvailable(const std::string& key) {
    return !SettingsChanges::ScalingMatchSetting(key) || ModLoading::Instance().HasSessionFeature("scaling");
}

bool NormalizeCampaignMatchSetting(const std::string& key, const std::string& value, std::string* normalized) {
    if (!SettingsChanges::CampaignMatchSetting(key) || value.size() > 64) return false;
    if (key == "difficulty_preset") {
        const auto presets = config.GetDifficultyPresets();
        if (std::find(presets.begin(), presets.end(), value) == presets.end()) return false;
        *normalized = value;
    } else if (key == "campaign_ai_scaling_rate") {
        if (value.size() != 1 || value[0] < '0' || value[0] > '4') return false;
        *normalized = value;
    } else if (key == "armor_localized" && value == "2") {
        *normalized = value;
    } else if (key == "game_difficulty" || key == "global_time_scale_mult") {
        std::istringstream input(value);
        float number;
        if (!(input >> number) || !std::isfinite(number)) return false;
        input >> std::ws;
        if (!input.eof() || number < (key == "game_difficulty" ? 0.0f : 0.1f) ||
            number > (key == "game_difficulty" ? 1.0f : 2.0f)) return false;
        *normalized = StringVariant(number).str();
    } else {
        if (value != "0" && value != "1" && value != "false" && value != "true") return false;
        *normalized = value == "1" || value == "true" ? "1" : "0";
    }
    return true;
}

void ApplyCampaignMatchSnapshot(const Json::Value& settings) {
    if (!settings.isObject()) return;
    bool changed = false;
    for (const char* key : campaign_match_settings) {
        std::string value;
        if (!MatchSettingAvailable(key) || !settings[key].isString() || !NormalizeCampaignMatchSetting(key, settings[key].asString(), &value)) continue;
        if (config[key].str() != value) {
            config.GetRef(key) = value;
            changed = true;
        }
    }
    if (changed) config.ReloadDynamicSettings();
}
void SeedLocalView(SceneGraph* graph, const PlayerState& player, ObjectID anchor_id) {
    Object* target = graph ? graph->GetObjectFromID(player.object_id) : nullptr;
    Object* anchor_object = graph ? graph->GetObjectFromID(anchor_id) : nullptr;
    if (!target || target->GetType() != _movement_object || !anchor_object || anchor_object->GetType() != _movement_object) return;
    auto* avatar = static_cast<MovementObject*>(target);
    auto* anchor = static_cast<MovementObject*>(anchor_object);
    const int camera_id = std::max(0, player.screen - 1);
    const int previous = ActiveCameras::GetID();
    Camera* source = ActiveCameras::GetCamera(anchor->camera_id);
    ActiveCameras::Set(camera_id);
    Camera* destination = ActiveCameras::Get();
    if (source && destination && source != destination) {
        *destination = *source;
        destination->m_camera_object = nullptr;
        destination->UnlockView();
        destination->SetPos(source->GetPos() + avatar->position - anchor->position);
        destination->FixDiscontinuity();
        destination->SetVelocity(vec3(0.0f));
    }
    avatar->camera_id = camera_id;
    ActiveCameras::Set(previous);
}
}

PeerID Online::LocalPeerID() const {
    return IsHosting() || online_session == nullptr ? 255 : static_cast<PeerID>(online_session->local_player_id);
}

std::vector<LobbyAuthority::Seat> Online::LobbySeats(bool include_bots) const {
    std::vector<LobbyAuthority::Seat> seats;
    if (online_session) for (const auto& entry : online_session->player_states)
        if (include_bots || !entry.second.is_bot) seats.push_back({entry.first, entry.second.peer_id, entry.second.local_index});
    if (include_bots) for (const auto& entry : lobby_slots_)
        if (!online_session || !online_session->player_states.count(entry.first))
            seats.push_back({entry.first, 255, std::numeric_limits<uint32_t>::max()});
    return seats;
}

PlayerID Online::LobbyPlayerIDForLocalIndex(unsigned index) const {
    if (online_session) {
        for (const auto& entry : online_session->player_states)
            if (!entry.second.is_bot && entry.second.peer_id == LocalPeerID() && entry.second.local_index == index) return entry.first;
        return LobbyAuthority::InvalidPlayer;
    }
    return 255 + index;
}

PlayerID Online::LocalLobbyLeaderPlayerID() const {
    const auto found = lobby_authority_.leaders.find(LocalPeerID());
    return found == lobby_authority_.leaders.end() ? LobbyPlayerIDForLocalIndex(0) : found->second;
}

PlayerID Online::ResolveLobbyActor(PlayerID actor) const {
    return actor == LobbyAuthority::InvalidPlayer ? LocalLobbyLeaderPlayerID() : actor;
}

bool Online::IsLobbyHost(PlayerID id) const {
    return id != LobbyAuthority::InvalidPlayer && (IsActive() ? id == lobby_authority_.host : id == 255);
}

bool Online::IsLobbyLeader(PlayerID id) const {
    return IsActive() ? lobby_authority_.IsLeader(LobbySeats(), id) : id == 255;
}

void Online::RefreshLobbyAuthorities() {
    if (!IsHosting() || !online_session) return;
    const auto seats = LobbySeats();
    lobby_authority_.Refresh(seats);
    const bool reserved = LobbyMigrationBusy() && migration_saved_players_.count(campaign_main_player_id_) != 0;
    const auto main = online_session->player_states.find(campaign_main_player_id_);
    const int main_team = CampaignMainLobbyTeam();
    const bool campaign_affiliation = lobby_force_teams_ && !CampaignTeamAffiliation(main_team).empty();
    const bool opposing_fallback = campaign_affiliation && main != online_session->player_states.end() && main->second.team != main_team;
    if ((main == online_session->player_states.end() || main->second.is_bot || opposing_fallback) && !reserved) {
        const PlayerState* replacement = nullptr;
        for (const auto& entry : online_session->player_states) {
            const auto& player = entry.second;
            if (player.is_bot || (campaign_affiliation && player.team != main_team)) continue;
            if (!replacement || (player.peer_id == 255 && replacement->peer_id != 255) ||
                (player.peer_id == replacement->peer_id && player.local_index < replacement->local_index)) replacement = &player;
        }
        campaign_main_player_id_ = replacement ? replacement->player_id : LobbyAuthority::InvalidPlayer;
        campaign_main_avatar_id_ = replacement ? replacement->object_id : static_cast<ObjectID>(-1);
    }
}

bool Online::CanPromoteLobbyHost(PlayerID target, PlayerID actor) const {
    return lobby_waiting_ && lobby_authority_.CanPromoteHost(LobbySeats(), ResolveLobbyActor(actor), target);
}

bool Online::PromoteLobbyHost(PlayerID target, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !CanPromoteLobbyHost(target, actor)) return false;
    Json::Value request; request["action"] = "promote_host"; request["actor"] = actor; request["id"] = target;
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::CanPromoteLobbyLeader(PlayerID target, PlayerID actor) const {
    return !LobbyMigrationBusy() && lobby_waiting_ && lobby_authority_.CanPromoteLeader(LobbySeats(), ResolveLobbyActor(actor), target);
}

bool Online::PromoteLobbyLeader(PlayerID target, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!CanPromoteLobbyLeader(target, actor)) return false;
    Json::Value request; request["action"] = "promote_leader"; request["actor"] = actor; request["id"] = target;
    SubmitLobbyRequest(Encode(request)); return true;
}

std::vector<std::string> Online::LocalLobbyNames() const {
    std::vector<std::string> names;
    for (const auto& profile : local_lobby_profiles_) names.push_back(profile.name);
    if (names.empty()) names.push_back(OnlineUtility::GetPlayerName());
    return names;
}

std::vector<int> Online::LocalLobbyScreens() const {
    std::vector<int> screens;
    for (const auto& profile : local_lobby_profiles_) screens.push_back(profile.screen);
    if (screens.empty()) screens.push_back(1);
    return screens;
}

bool Online::HasMultipleLobbyHumans() const {
    if (!online_session) return false;
    int humans = 0;
    for (const auto& entry : online_session->player_states) {
        if (entry.second.connected && !entry.second.is_bot && ++humans > 1) return true;
    }
    return false;
}

bool Online::AllowsSoloCinematics() const {
    if (!IsActive()) return !IsAwaitingShutdown();
    if (!IsHosting() || LobbyWaiting()) return false;
    return std::count_if(online_session->player_states.begin(), online_session->player_states.end(),
        [](const auto& entry) { return entry.second.connected && !entry.second.is_bot; }) == 1;
}

unsigned Online::LocalLobbyPlayerCount() const {
    const unsigned profiles = std::max(1u, static_cast<unsigned>(local_lobby_profiles_.size()));
    if (IsActive() && !lobby_waiting_ && online_session != nullptr) {
        unsigned count = 0;
        for (const auto& entry : online_session->player_states) if (!entry.second.is_bot && entry.second.peer_id == LocalPeerID()) ++count;
        return std::max(profiles, count);
    }
    return profiles;
}

ObjectID Online::CampaignMainAvatarID() const {
    if (!online_session) return static_cast<ObjectID>(-1);
    const auto main = online_session->player_states.find(campaign_main_player_id_);
    if (main != online_session->player_states.end() && main->second.object_id != static_cast<ObjectID>(-1)) return main->second.object_id;
    return campaign_main_avatar_id_;
}

bool Online::CanAdoptLocalSession() const {
    return mode.load() == MultiplayerMode::NoMultiplayer && online_session == nullptr;
}

bool Online::EnsureLocalSession() {
    if (IsActive()) return true;
    // AwaitingShutdown is deliberately inactive, but its old session and
    // worker still exist until StopMultiplayer finishes on the main thread.
    if (!CanAdoptLocalSession()) return false;
    Engine* engine = Engine::Instance();
    SceneGraph* graph = engine->GetSceneGraph();
    const bool loaded = engine->IsLevelLoaded() && graph && graph->level;
    const bool loading_world = engine->current_engine_state_.type == kEngineLevelState ||
        engine->current_engine_state_.type == kEngineEditorLevelState;
    if (!loaded && !loading_world) return false;
    if (loaded && graph->map_editor->state_ != MapEditor::kInGame) return false;
    std::vector<ObjectID> existing;
    if (loaded) graph->GetPlayerCharacterIDs(existing);
    while (local_lobby_profiles_.size() < std::max<size_t>(1, existing.size())) {
        LocalLobbyProfile profile;
        profile.name = local_lobby_profiles_.empty() ? OnlineUtility::GetPlayerName() : NameOrDefault("", static_cast<unsigned>(local_lobby_profiles_.size()));
        profile.screen = static_cast<int>(local_lobby_profiles_.size()) + 1;
        local_lobby_profiles_.push_back(profile);
    }
    lobby_waiting_ = false;
    lobby_authority_ = LobbyAuthority::State();
    StartMultiplayer(MultiplayerMode::Host, true);  // adopt the current world, without opening a listen socket
    ResetModeChallenge();  // a fresh private run must not inherit a closed session's ledger
    local_only_session_ = true;
    loading = !loaded;
    lobby_start_pending_ = !loaded;
    lobby_map_.clear();  // Direct Play did not select a level from a lobby.
    lobby_build_ = false;
    if (ScriptableCampaign* campaign = engine->GetCurrentCampaign()) {
        campaign_id = campaign->GetCampaignID();
        lobby_mode_ = lobby_campaign_ = campaign_id;
    } else {
        lobby_mode_ = "local"; lobby_campaign_.clear();
    }
    campaign_main_player_id_ = LobbyPlayerIDForLocalIndex(0);
    campaign_main_avatar_id_ = existing.empty() ? static_cast<ObjectID>(-1) : existing.front();
    if (!existing.empty()) CaptureNaturalPlayerTeam(graph, level_name, campaign_main_avatar_id_);
    for (const auto& player : GetLocalPlayerStates()) if (player.local_index < existing.size()) {
        PossessAvatar(player.player_id, existing[player.local_index]);
        RemoveFreeAvatarId(existing[player.local_index]);
        initialized_appearances[player.player_id] = existing[player.local_index];
        PlayerState adopted = player; adopted.object_id = existing[player.local_index];
        SeedLocalView(graph, adopted, campaign_main_avatar_id_);
    }
    if (loaded) AssignPeerAvatars(LocalPeerID());
    engine->CommitPause();  // recompute after adopting the local-only session policy
    BroadcastLobby();
    return true;
}

bool Online::OpenLobbyToNetwork(PlayerID actor) {
    if (!IsHosting() || !IsLobbyHost(ResolveLobbyActor(actor))) return false;
    StartHostingMultiplayer();
    return !local_only_session_;
}

std::vector<PlayerState> Online::GetLocalPlayerStates() const {
    std::vector<PlayerState> result;
    if (online_session != nullptr) {
        for (const auto& entry : online_session->player_states) {
            if (!entry.second.is_bot && entry.second.peer_id == LocalPeerID()) {
                result.push_back(entry.second);
                result.back().player_id = entry.first;
                result.back().controller_id = Input::Instance()->GetLocalPlayerInputID(entry.second.local_index);
                result.back().camera_id = std::max(0, entry.second.screen - 1);
            }
        }
    } else {
        auto names = LocalLobbyNames();
        for (size_t i = 0; i < names.size(); ++i) {
            PlayerState state;
            state.player_id = i == 0 ? 255 : static_cast<PlayerID>(255 + i);
            state.peer_id = 255;
            state.local_index = static_cast<uint32_t>(i);
            state.screen = i < local_lobby_profiles_.size() ? local_lobby_profiles_[i].screen : static_cast<int>(i + 1);
            state.playername = names[i];
            state.object_id = -1;
            result.push_back(state);
        }
    }
    std::sort(result.begin(), result.end(), [](const PlayerState& a, const PlayerState& b) { return a.local_index < b.local_index; });
    return result;
}

bool Online::BeginLobby(bool local_only) {
    if (IsActive()) return IsHosting() && lobby_waiting_;
    if (local_lobby_profiles_.empty()) {
        LocalLobbyProfile profile;
        profile.name = OnlineUtility::GetPlayerName();
        local_lobby_profiles_.push_back(profile);
    }
    lobby_waiting_ = true;
    lobby_start_pending_ = false;
    lobby_mode_.clear(); lobby_campaign_.clear(); lobby_map_.clear(); lobby_build_ = false;
    lobby_allow_all_pause_ = false;
    lobby_force_teams_ = false;
    lobby_host_controls_teams_ = true;
    lobby_authority_ = LobbyAuthority::State();
    mode_teams_.clear();
    level_name.clear(); campaign_id.clear();
    if (local_only) {
        StartMultiplayer(MultiplayerMode::Host);
        local_only_session_ = true;
    } else {
        StartHostingMultiplayer();
    }
    EstablishPeerPlayers(255, LocalLobbyNames());
    return IsHosting();
}

PlayerID Online::AddLocalLobbyPlayer(const std::string& name) {
    if (LobbyMigrationBusy()) return static_cast<PlayerID>(-1);
    if (IsHosting() && !lobby_auto_admission_) {
        const bool available = std::any_of(lobby_slots_.begin(), lobby_slots_.end(), [this](const std::pair<const PlayerID, LobbySlot>& slot) {
            return !slot.second.is_bot && !online_session->player_states.count(slot.first);
        });
        if (!available) return LobbyAuthority::InvalidPlayer;
    }
    if (IsClient() && !lobby_auto_admission_) {
        const auto players = GetPlayerStates();
        const bool available = std::any_of(lobby_slots_.begin(), lobby_slots_.end(), [&players](const std::pair<const PlayerID, LobbySlot>& slot) {
            return !slot.second.is_bot && std::none_of(players.begin(), players.end(), [&slot](const PlayerState& p) { return p.player_id == slot.first; });
        });
        if (!available) return LobbyAuthority::InvalidPlayer;
    }
    Engine* engine = Engine::Instance();
    const bool world = engine->IsLevelLoaded() || engine->current_engine_state_.type == kEngineLevelState ||
        engine->current_engine_state_.type == kEngineEditorLevelState;
    if (!IsActive() && world && !EnsureLocalSession()) return static_cast<PlayerID>(-1);
    if (local_lobby_profiles_.empty()) {
        LocalLobbyProfile primary;
        primary.name = OnlineUtility::GetPlayerName();
        local_lobby_profiles_.push_back(primary);
    }
    LocalLobbyProfile profile;
    profile.name = NameOrDefault(name, static_cast<unsigned>(local_lobby_profiles_.size()));
    profile.screen = static_cast<int>(local_lobby_profiles_.size()) + 1;
    local_lobby_profiles_.push_back(profile);
    Input::Instance()->SetUpForXPlayers(static_cast<unsigned>(local_lobby_profiles_.size()), IsActive());
    if (IsHosting()) {
        EstablishPeerPlayers(255, LocalLobbyNames());
        AssignPeerAvatars(LocalPeerID());
        BroadcastLobby();
        for (const auto& state : GetLocalPlayerStates()) {
            if (state.local_index + 1 == local_lobby_profiles_.size()) return state.player_id;
        }
    } else if (IsClient()) {
        Json::Value request;
        request["action"] = "join";
        request["name"] = profile.name;
        request["local_index"] = static_cast<Json::UInt>(local_lobby_profiles_.size() - 1);
        SubmitLobbyRequest(Encode(request));
    }
    return static_cast<PlayerID>(255 + local_lobby_profiles_.size() - 1);
}

bool Online::HasSpaceForLobbySlot() const {
    if (!online_session || next_local_player_id_ >= LobbyAuthority::InvalidPlayer - 1) return false;
    // The common case needs no allocation. At capacity, count the union of
    // persisted slots and active seats that have not been recorded yet.
    return lobby_slots_.size() + online_session->player_states.size() < LobbyAuthority::kMaxSlots ||
           GetLobbySlots().size() < LobbyAuthority::kMaxSlots;
}

PlayerID Online::AddLobbyBot(const std::string& name, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || (!name.empty() && !OnlineUtility::IsValidPlayerName(name))) return LobbyAuthority::InvalidPlayer;
    if (!IsActive() && !EnsureLocalSession()) return LobbyAuthority::InvalidPlayer;
    if (!IsHosting() || !IsLobbyHost(actor) || !HasSpaceForLobbySlot()) return LobbyAuthority::InvalidPlayer;
    PlayerState bot;
    bot.player_id = LobbyAuthority::AllocatePlayerID(LobbySeats(true), 255, 1, next_local_player_id_);
    bot.peer_id = 255;
    bot.local_index = std::numeric_limits<uint32_t>::max();
    bot.is_bot = true;
    bot.screen = 0; bot.controller_id = -1; bot.camera_id = -1; bot.object_id = -1;
    bot.playername = name;
    if (bot.playername.empty()) {
        unsigned number = 1;
        do { bot.playername = "Bot " + std::to_string(number++); }
        while (std::any_of(online_session->player_states.begin(), online_session->player_states.end(), [&bot](const std::pair<const PlayerID, PlayerState>& seat) {
            return seat.second.playername == bot.playername;
        }));
    }
    bot.team = lobby_mode_ == "com-wolfire-arena" || lobby_campaign_ == "com-wolfire-arena" ?
        static_cast<int>(online_session->player_states.size() + 1) : CampaignMainLobbyTeam();
    online_session->player_states[bot.player_id] = bot;
    AssignLobbyBots();
    BroadcastLobby();
    return bot.player_id;
}

bool Online::EstablishPeerPlayers(PeerID peer, const std::vector<std::string>& names, const std::vector<int>& requested_screens) {
    if (!IsHosting() || online_session == nullptr) return false;
    const size_t count = std::max<size_t>(1, names.size());
    SyncLobbySlotDefinitions();
    size_t additions = 0;
    for (size_t index = 0; index < count; ++index)
        if (std::none_of(online_session->player_states.begin(), online_session->player_states.end(), [peer,index](const std::pair<const PlayerID,PlayerState>& p) {
            return !p.second.is_bot && p.second.peer_id == peer && p.second.local_index == index;
        })) ++additions;
    size_t available = 0;
    for (const auto& slot : lobby_slots_) if (!slot.second.is_bot && !online_session->player_states.count(slot.first)) ++available;
    if (!lobby_auto_admission_ && additions > available) return false;
    std::vector<int> screens = peer == LocalPeerID() ? LocalLobbyScreens() : requested_screens;
    std::set<int> unique_screens(screens.begin(), screens.end());
    const bool valid_screens = screens.size() == count && unique_screens.size() == count &&
        *unique_screens.begin() == 1 && *unique_screens.rbegin() == static_cast<int>(count);
    int couch_team = CampaignMainLobbyTeam();
    const PlayerState* couch_primary = nullptr;
    const auto leader = lobby_authority_.leaders.find(peer);
    for (const auto& entry : online_session->player_states) {
        const auto& member = entry.second;
        if (member.is_bot || member.peer_id != peer) continue;
        if (!couch_primary || (leader != lobby_authority_.leaders.end() && member.player_id == leader->second) ||
            (member.local_index < couch_primary->local_index &&
             (leader == lobby_authority_.leaders.end() || couch_primary->player_id != leader->second))) couch_primary = &member;
    }
    if (couch_primary) couch_team = couch_primary->team;
    for (size_t index = 0; index < count; ++index) {
        auto found = std::find_if(online_session->player_states.begin(), online_session->player_states.end(),
            [peer, index](const std::pair<const PlayerID, PlayerState>& p) { return !p.second.is_bot && p.second.peer_id == peer && p.second.local_index == index; });
        if (found != online_session->player_states.end()) continue;
        PlayerID id = LobbyAuthority::InvalidPlayer;
        for (const auto& slot : GetLobbySlots()) if (!slot.is_bot && !online_session->player_states.count(slot.id)) { id = slot.id; break; }
        if (id == LobbyAuthority::InvalidPlayer)
            id = LobbyAuthority::AllocatePlayerID(LobbySeats(true), peer, static_cast<unsigned>(index), next_local_player_id_);
        PlayerState state;
        state.player_id = id;
        state.peer_id = peer;
        state.local_index = static_cast<uint32_t>(index);
        state.playername = NameOrDefault(index < names.size() ? names[index] : "", static_cast<unsigned>(index));
        state.object_id = -1;
        state.screen = valid_screens ? screens[index] : static_cast<int>(index + 1);
        state.team = lobby_slots_.count(id) ? lobby_slots_.at(id).team :
            lobby_mode_ == "com-wolfire-arena" || lobby_campaign_ == "com-wolfire-arena" ?
                static_cast<int>(lobby_slots_.size() + 1) : couch_team;
        online_session->player_states[id] = state;
        if (peer == LocalPeerID()) {
            online_session->player_states[id].controller_id = Input::Instance()->GetLocalPlayerInputID(static_cast<unsigned>(index));
            online_session->player_states[id].camera_id = state.screen - 1;
        } else {
            AssignNewControllerForPlayer(id);
        }
    }
    BroadcastLobby();
    return true;
}

void Online::AssignPeerAvatars(PeerID peer) {
    if (!IsHosting() || loading || lobby_waiting_ || !Engine::Instance()->IsLevelLoaded()) return;
    std::vector<PlayerID> ids;
    for (const auto& entry : online_session->player_states) if (!entry.second.is_bot && entry.second.peer_id == peer) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end(), [this](PlayerID a, PlayerID b) {
        if (a == campaign_main_player_id_) return b != campaign_main_player_id_;
        if (b == campaign_main_player_id_) return false;
        return online_session->player_states[a].local_index < online_session->player_states[b].local_index;
    });
    for (PlayerID id : ids) {
        auto& state = online_session->player_states[id];
        if (state.object_id != static_cast<ObjectID>(-1)) {
            RemoveFreeAvatarId(state.object_id);
            continue;
        }
        ObjectID avatar = static_cast<ObjectID>(-1);
        auto& available = online_session->free_avatars;
        auto chosen = std::find_if(available.begin(), available.end(), [this, id](ObjectID candidate) {
            return id == campaign_main_player_id_ ? candidate == campaign_main_avatar_id_ : candidate != campaign_main_avatar_id_;
        });
        if (chosen == available.end() && id == campaign_main_player_id_) chosen = available.begin();
        if (chosen != available.end()) {
            avatar = *chosen; available.erase(chosen); online_session->taken_avatars.push_back(avatar);
        }
        if (avatar == static_cast<ObjectID>(-1)) avatar = CreateCharacter();
        if (avatar == static_cast<ObjectID>(-1)) continue;
        SceneGraph* graph = Engine::Instance()->GetSceneGraph();
        CaptureNaturalPlayerTeam(graph, level_name, avatar);
        PossessAvatar(id, avatar);
        if (id == campaign_main_player_id_) campaign_main_avatar_id_ = avatar;
        if (peer == LocalPeerID()) SeedLocalView(graph, state, CampaignMainAvatarID());
        ApplyAssignedTeam(avatar);
    }
    AssignLobbyBots();
}

void Online::SyncLobbySlotDefinitions() {
    if (!online_session) return;
    uint32_t order = 0;
    for (const auto& entry : lobby_slots_) order = std::max(order, entry.second.order);
    for (const auto& entry : online_session->player_states) {
        auto& slot = lobby_slots_[entry.first];
        slot.id = entry.first;
        if (slot.uid.empty()) slot.uid = NewSlotUID(slot.id);
        if (!slot.order) slot.order = ++order;
        slot.name = entry.second.playername;
        slot.team = entry.second.team;
        slot.is_bot = entry.second.is_bot;
    }
}

std::vector<LobbySlot> Online::GetLobbySlots() const {
    auto slots = lobby_slots_;
    if (online_session) for (const auto& entry : online_session->player_states) {
        auto& slot = slots[entry.first];
        slot.id = entry.first; slot.name = entry.second.playername; slot.team = entry.second.team; slot.is_bot = entry.second.is_bot;
    }
    std::vector<LobbySlot> result;
    for (const auto& entry : slots) result.push_back(entry.second);
    std::sort(result.begin(), result.end(), [](const LobbySlot& a, const LobbySlot& b) { return a.order != b.order ? a.order < b.order : a.id < b.id; });
    return result;
}

Json::Value Online::SaveLobbySlots() const {
    Json::Value result;
    result["version"] = 1;
    result["auto_admit"] = lobby_auto_admission_;
    result["host_slot"] = lobby_authority_.host;
    result["next_slot_id"] = next_local_player_id_;
    result["slots"] = Json::Value(Json::arrayValue);
    for (const auto& slot : GetLobbySlots()) {
        Json::Value row;
        row["id"] = slot.id; row["uid"] = slot.uid; row["order"] = slot.order;
        row["kind"] = slot.is_bot ? "bot" : "player";
        row["name"] = slot.name; row["team"] = slot.team;
        result["slots"].append(row);
    }
    return result;
}

bool Online::StageLobbySlots(const Json::Value& saved, std::map<PlayerID, LobbySlot>& slots,
                             std::map<PlayerID, PlayerState>& players, LobbyAuthority::State& authority, PlayerID& next) const {
    if (!IsHosting() || !online_session || !DecodeSlotDefinitions(saved, slots, next)) return false;
    const PlayerID host = saved["host_slot"].asUInt();
    std::set<std::string> identities;
    for (const auto& entry : slots) identities.insert(entry.second.uid);
    std::vector<PlayerState> humans;
    for (const auto& entry : online_session->player_states) if (!entry.second.is_bot) humans.push_back(entry.second);
    std::sort(humans.begin(), humans.end(), [this](const PlayerState& a, const PlayerState& b) {
        if (a.player_id == lobby_authority_.host) return b.player_id != lobby_authority_.host;
        if (b.player_id == lobby_authority_.host) return false;
        return a.peer_id != b.peer_id ? a.peer_id < b.peer_id : a.local_index < b.local_index;
    });
    if (humans.empty()) return false;
    std::set<PlayerID> occupied;
    std::map<PlayerID, PlayerID> reassigned;
    for (size_t i = 0; i < humans.size(); ++i) {
        const auto& human = humans[i];
        PlayerID target = i == 0 ? host : LobbyAuthority::InvalidPlayer;
        const auto old = lobby_slots_.find(human.player_id);
        if (target == LobbyAuthority::InvalidPlayer && old != lobby_slots_.end())
            for (const auto& entry : slots) if (!entry.second.is_bot && !occupied.count(entry.first) && entry.second.uid == old->second.uid) target = entry.first;
        if (target == LobbyAuthority::InvalidPlayer)
            for (const auto& entry : slots) if (!entry.second.is_bot && !occupied.count(entry.first)) { target = entry.first; break; }
        if (target == LobbyAuthority::InvalidPlayer) {
            if (!saved["auto_admit"].asBool() || slots.size() >= LobbyAuthority::kMaxSlots || next >= LobbyAuthority::InvalidPlayer - 1) return false;
            target = next++;
            LobbySlot extra;
            extra.id = target; extra.uid = old != lobby_slots_.end() ? old->second.uid : "restored-" + std::to_string(target);
            for (const auto& entry : slots) extra.order = std::max(extra.order, entry.second.order);
            ++extra.order;
            if (!identities.insert(extra.uid).second) extra.uid += "-" + std::to_string(target);
            extra.name = human.playername; extra.team = human.team; extra.is_bot = false;
            slots[target] = extra;
        }
        occupied.insert(target);
        PlayerState bound = human;
        bound.player_id = target; bound.team = slots.at(target).team; bound.object_id = -1;
        players[target] = bound;
        reassigned[human.player_id] = target;
    }
    for (const auto& entry : slots) if (entry.second.is_bot) {
        PlayerState bot;
        bot.player_id = entry.first; bot.is_bot = true; bot.playername = entry.second.name; bot.team = entry.second.team;
        bot.peer_id = 255; bot.local_index = std::numeric_limits<uint32_t>::max(); bot.screen = 0; bot.controller_id = bot.camera_id = -1; bot.object_id = -1;
        players[entry.first] = bot;
    }
    authority.host = host;
    for (const auto& leader : lobby_authority_.leaders) if (reassigned.count(leader.second)) authority.leaders[leader.first] = reassigned.at(leader.second);
    std::vector<LobbyAuthority::Seat> seats;
    for (const auto& entry : players) if (!entry.second.is_bot) seats.push_back({entry.first, entry.second.peer_id, entry.second.local_index});
    authority.Refresh(seats);
    return true;
}

bool Online::ValidateLobbySlots(const Json::Value& saved) const {
    std::map<PlayerID, LobbySlot> slots;
    if (!IsActive()) {
        PlayerID next;
        if (!DecodeSlotDefinitions(saved, slots, next)) return false;
        const size_t humans = std::max<size_t>(1, local_lobby_profiles_.size());
        size_t available = 0;
        for (const auto& entry : slots) if (!entry.second.is_bot) ++available;
        return humans <= available || (saved["auto_admit"].asBool() && slots.size() + humans - available <= LobbyAuthority::kMaxSlots);
    }
    std::map<PlayerID, PlayerState> players;
    LobbyAuthority::State authority;
    PlayerID next;
    return StageLobbySlots(saved, slots, players, authority, next);
}

bool Online::RestoreLobbySlots(const Json::Value& saved) {
    if (!LobbyWaiting() || LobbyMigrationBusy() || Engine::Instance()->IsLevelLoaded()) return false;
    std::map<PlayerID, LobbySlot> slots;
    std::map<PlayerID, PlayerState> players;
    LobbyAuthority::State authority;
    PlayerID next;
    if (!StageLobbySlots(saved, slots, players, authority, next)) return false;
    for (const auto& entry : online_session->player_states) ReleaseMenuPause(entry.first);
    lobby_slots_ = std::move(slots);
    online_session->player_states.clear();
    online_session->player_states.insert(players.begin(), players.end());
    online_session->player_inputs.clear();
    lobby_authority_ = std::move(authority);
    lobby_auto_admission_ = saved["auto_admit"].asBool();
    next_local_player_id_ = next;
    initialized_appearances.clear();
    if (!lobby_slots_.count(campaign_main_player_id_)) campaign_main_player_id_ = lobby_authority_.host;
    campaign_main_avatar_id_ = -1;
    ApplyLocalLobbyScreens();
    BroadcastLobby();
    return true;
}

bool Online::SetLobbyAutoAdmission(bool automatic, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!LobbyWaiting() || LobbyMigrationBusy() || !IsLobbyHost(actor)) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "slot_admission"; request["actor"] = actor; request["automatic"] = automatic;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    if (!IsHosting()) return false;
    lobby_auto_admission_ = automatic;
    BroadcastLobby();
    return true;
}

bool Online::SetLobbySlotPlayer(PlayerID id, bool player, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!LobbyWaiting() || LobbyMigrationBusy() || !IsLobbyHost(actor) || !lobby_slots_.count(id)) return false;
    const auto occupied = online_session->player_states.find(id);
    if (occupied != online_session->player_states.end() && !occupied->second.is_bot) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "slot_kind"; request["actor"] = actor; request["id"] = id; request["player"] = player;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    auto& slot = lobby_slots_.at(id);
    if (slot.is_bot == !player) return true;
    if (player) {
        online_session->player_states.erase(id);
        online_session->player_inputs.erase(id);
        Send<OnlineMessages::RemovePlayerState>(id);
    } else {
        PlayerState bot;
        bot.player_id = id; bot.playername = slot.name; bot.team = slot.team; bot.is_bot = true;
        bot.local_index = std::numeric_limits<uint32_t>::max(); bot.object_id = -1; bot.screen = 0; bot.controller_id = bot.camera_id = -1;
        online_session->player_states[id] = bot;
    }
    slot.is_bot = !player;
    BroadcastLobby();
    return true;
}

bool Online::SwapLobbySlots(PlayerID first, PlayerID second, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!LobbyWaiting() || LobbyMigrationBusy() || !IsLobbyHost(actor) || first == second ||
        !lobby_slots_.count(first) || !lobby_slots_.count(second)) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "slot_swap"; request["actor"] = actor; request["first"] = first; request["second"] = second;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    std::map<PlayerID, PlayerState> occupants;
    for (PlayerID id : {first, second}) {
        const auto found = online_session->player_states.find(id);
        if (found != online_session->player_states.end()) occupants[id] = found->second;
    }
    if (occupants.empty()) return false;
    for (PlayerID target : {first, second}) {
        const PlayerID source = target == first ? second : first;
        online_session->player_states.erase(target);
        online_session->player_inputs.erase(target);
        auto& slot = lobby_slots_.at(target);
        if (occupants.count(source)) {
            PlayerState bound = occupants.at(source);
            bound.player_id = target; bound.team = slot.team; bound.object_id = -1;
            online_session->player_states[target] = bound;
            slot.is_bot = bound.is_bot;
            slot.name = bound.playername;
        } else {
            slot.is_bot = false;
        }
    }
    auto moved = [first, second](PlayerID id) { return id == first ? second : id == second ? first : id; };
    lobby_authority_.host = moved(lobby_authority_.host);
    for (auto& leader : lobby_authority_.leaders) leader.second = moved(leader.second);
    ApplyLocalLobbyScreens();
    BroadcastLobby();
    return true;
}

std::string Online::LobbyPayload() const {
    Json::Value message;
    message["action"] = "snapshot";
    message["waiting"] = lobby_waiting_;
    message["mode"] = lobby_mode_; message["campaign"] = lobby_campaign_; message["map"] = lobby_map_;
    message["build"] = lobby_build_; message["allow_all_pause"] = lobby_allow_all_pause_;
    message["force_teams"] = lobby_force_teams_;
    message["mode_settings"] = LobbyModeSettings();
    message["session_mods"] = SessionMods::Snapshot(SessionMods::Scope::Network);
    message["host_controls_teams"] = lobby_host_controls_teams_;
    message["slot_definitions"] = SaveLobbySlots();
    message["match_settings"] = Json::Value(Json::objectValue);
    for (const char* key : campaign_match_settings)
        if (MatchSettingAvailable(key)) message["match_settings"][key] = config[key].str();
    message["decision_host"] = lobby_authority_.host;
    SceneGraph* scene = Engine::Instance()->GetSceneGraph();
    message["cutscene"] = host_started_level && !loading && scene && scene->level && scene->level->DialogueCameraControl();
    message["campaign_main_player"] = campaign_main_player_id_;
    message["campaign_main_object"] = static_cast<int>(GetOriginalID(campaign_main_avatar_id_));
    message["leaders"] = Json::Value(Json::arrayValue);
    for (const auto& leader : lobby_authority_.leaders) {
        Json::Value row; row["peer"] = leader.first; row["player"] = leader.second;
        message["leaders"].append(row);
    }
    message["natural_team"] = natural_player_team;
    message["campaign_factions"]["level"] = level_name;
    message["campaign_factions"]["main_tags"] = CampaignFactionMainTags();
    message["campaign_factions"]["main_team"] = CampaignMainLobbyTeam();
    message["campaign_factions"]["anchor_player"] = CampaignFactionAnchorPlayer();
    message["campaign_factions"]["prefix"] = CampaignFactionPrefix();
    message["campaign_factions"]["enemies"] = Json::Value(Json::arrayValue);
    for (const auto& profile : CampaignEnemyFactionProfiles()) message["campaign_factions"]["enemies"].append(profile);
    message["mode_teams"] = Json::Value(Json::arrayValue);
    for (const auto& assignment : mode_teams_) {
        Json::Value row; row["object"] = assignment.first; row["team"] = assignment.second;
        message["mode_teams"].append(row);
    }
    message["frozen"] = time_frozen_;
    message["players"] = Json::Value(Json::arrayValue);
    if (online_session != nullptr) for (const auto& entry : online_session->player_states) {
        const auto& p = entry.second;
        Json::Value row;
        row["id"] = entry.first; row["peer"] = p.peer_id; row["local_index"] = p.local_index;
        row["name"] = p.playername; row["team"] = p.team; row["screen"] = p.screen; row["connected"] = p.connected;
        row["bot"] = p.is_bot;
        row["object"] = static_cast<int>(GetOriginalID(p.object_id));
        row["controller"] = p.controller_id; row["camera"] = p.camera_id;
        message["players"].append(row);
    }
    return Encode(message);
}

void Online::BroadcastLobby() {
    if (!IsHosting() || online_session == nullptr) return;
    SyncLobbySlotDefinitions();
    RefreshLobbyAuthorities();
    // Team IDs also own shared progression wallets and campaign affiliation.
    // Keep their identity when a removed unit leaves a gap in the roster.
    Send<OnlineMessages::LobbyControl>(LobbyPayload());
}

std::string Online::ExportLobbyForMigration() const {
    Json::Value message; Json::Reader reader;
    reader.parse(LobbyPayload(), message, false);
    message["match_settings"] = Json::Value(Json::objectValue);
    for (const char* key : migration_match_settings)
        if (MatchSettingAvailable(key) && config.HasKey(key)) message["match_settings"][key] = config.map_.at(key).data.str();
    message["default_hot_join_character"] = default_hot_join_characte_path;
    message["saved_mods"] = SessionMods::Snapshot(SessionMods::Scope::Handoff);
    return Encode(message);
}

bool Online::RestoreLobbyForMigration(const std::string& payload, PeerID origin_local_peer, PlayerID target_host) {
    if (!IsHosting() || !online_session) return false;
    Json::Value message; Json::Reader reader;
    if (!reader.parse(payload, message, false) || !message["players"].isArray() || !message.get("waiting", false).asBool()) return false;
    std::string mod_error;
    if (!ReceiveLobbyModeSettings(message["mode_settings"], true) ||
        !SessionMods::Validate(message["session_mods"], SessionMods::Scope::Network, mod_error) ||
        !SessionMods::Validate(message["saved_mods"], SessionMods::Scope::Handoff, mod_error)) return false;
    std::map<PlayerID, LobbySlot> slot_definitions;
    PlayerID slot_next = 256;
    if (message.isMember("slot_definitions") && !DecodeSlotDefinitions(message["slot_definitions"], slot_definitions, slot_next)) return false;
    std::map<PlayerID, PlayerState> saved;
    for (const auto& row : message["players"]) {
        PlayerState player;
        player.player_id = row.get("id", 0).asUInt(); player.peer_id = static_cast<PeerID>(row.get("peer", 0).asUInt());
        player.local_index = row.get("local_index", 0).asUInt(); player.playername = row.get("name", "UNKNOWN").asString();
        player.team = row.get("team", 1).asInt(); player.screen = row.get("screen", 1).asInt();
        player.connected = row.get("connected", true).asBool(); player.object_id = -1;
        player.is_bot = row.get("bot", false).asBool();
        if (player.is_bot) {
            player.peer_id = 255; player.local_index = std::numeric_limits<uint32_t>::max();
            player.screen = 0; player.controller_id = -1; player.camera_id = -1;
        }
        if (!OnlineUtility::IsValidPlayerName(player.playername) || player.team < 1 || (!player.is_bot && player.screen < 1) || saved.count(player.player_id)) return false;
        saved[player.player_id] = player;
    }
    const auto target = saved.find(target_host);
    if (target == saved.end() || target->second.is_bot || target->second.peer_id != origin_local_peer) return false;
    migration_saved_players_ = saved;
    campaign_main_player_id_ = message.get("campaign_main_player", target_host).asUInt();
    campaign_main_avatar_id_ = static_cast<ObjectID>(-1);
    migration_saved_authority_ = LobbyAuthority::State();
    migration_saved_authority_.host = target_host;
    for (const auto& leader : message["leaders"])
        migration_saved_authority_.leaders[static_cast<PeerID>(leader.get("peer", 255).asUInt())] = leader.get("player", 255).asUInt();
    lobby_authority_ = migration_saved_authority_;
    lobby_authority_.leaders.clear();
    lobby_waiting_ = true; lobby_start_pending_ = false;
    lobby_mode_ = message.get("mode", "").asString(); lobby_campaign_ = message.get("campaign", "").asString();
    lobby_map_ = message.get("map", "").asString(); lobby_build_ = message.get("build", false).asBool();
    lobby_allow_all_pause_ = message.get("allow_all_pause", false).asBool();
    lobby_force_teams_ = message.get("force_teams", false).asBool();
    lobby_host_controls_teams_ = message.get("host_controls_teams", true).asBool();
    ReceiveLobbyModeSettings(message["mode_settings"]);
    SessionMods::Restore(message["saved_mods"], SessionMods::Scope::Handoff);
    SessionMods::Restore(message["session_mods"], SessionMods::Scope::Network);
    const Json::Value& settings = message["match_settings"];
    if (settings.isObject()) {
        for (const char* key : migration_match_settings)
            if (MatchSettingAvailable(key) && settings[key].isString() && settings[key].asString().size() <= 128)
                config.GetRef(key) = settings[key].asString();
        config.ReloadDynamicSettings();
    }
    const std::string default_character = message.get("default_hot_join_character", "").asString();
    if (!default_character.empty() && default_character.size() < 4096) default_hot_join_characte_path = default_character;
    online_session->player_states.clear();
    online_session->player_inputs.clear();
    mode_teams_.clear();
    next_local_player_id_ = 256;
    lobby_slots_ = std::move(slot_definitions);
    lobby_auto_admission_ = message.get("slot_definitions", Json::Value()).get("auto_admit", true).asBool();
    next_local_player_id_ = std::max(next_local_player_id_, slot_next);
    for (const auto& row : saved) if (row.first >= next_local_player_id_ && row.first != LobbyAuthority::InvalidPlayer) next_local_player_id_ = row.first + 1;
    for (const auto& row : saved) if (row.second.is_bot) online_session->player_states[row.first] = row.second;
    local_lobby_profiles_.clear();
    std::vector<PlayerState> local;
    for (const auto& row : saved) if (!row.second.is_bot && row.second.peer_id == origin_local_peer) local.push_back(row.second);
    std::sort(local.begin(), local.end(), [](const PlayerState& a, const PlayerState& b) { return a.local_index < b.local_index; });
    for (const auto& player : local) {
        LocalLobbyProfile profile; profile.name = player.playername; profile.screen = player.screen;
        local_lobby_profiles_.push_back(profile);
    }
    Input::Instance()->SetUpForXPlayers(static_cast<unsigned>(local.size()), true);
    return RestoreMigratedPeer(255, origin_local_peer);
}

bool Online::RestoreMigratedPeer(PeerID new_peer, PeerID old_peer) {
    if (!IsHosting() || !online_session) return false;
    bool restored = false;
    for (const auto& row : migration_saved_players_) {
        if (row.second.is_bot || row.second.peer_id != old_peer) continue;
        PlayerState player = row.second; player.peer_id = new_peer;
        online_session->player_states[row.first] = player;
        if (new_peer == LocalPeerID()) {
            auto& state = online_session->player_states[row.first];
            state.controller_id = Input::Instance()->GetLocalPlayerInputID(state.local_index);
            state.camera_id = state.screen - 1;
        } else AssignNewControllerForPlayer(row.first);
        restored = true;
    }
    const auto leader = migration_saved_authority_.leaders.find(old_peer);
    if (leader != migration_saved_authority_.leaders.end()) lobby_authority_.leaders[new_peer] = leader->second;
    if (restored) BroadcastLobby();
    return restored;
}

void Online::SubmitLobbyRequest(const std::string& payload) {
    if (IsHosting()) ReceiveLobbyControl(255, payload);
    else if (IsClient()) Send<OnlineMessages::LobbyControl>(payload);
}

bool Online::CanRenameLobbyPlayer(PlayerID id, PlayerID actor) const {
    if (LobbyMigrationBusy() || !IsActive() || !online_session) return false;
    actor = ResolveLobbyActor(actor);
    const auto found = online_session->player_states.find(id);
    const auto owner = online_session->player_states.find(actor);
    if (found == online_session->player_states.end() && lobby_slots_.count(id)) return LobbyWaiting() && IsLobbyHost(actor);
    if (found == online_session->player_states.end() || owner == online_session->player_states.end() || owner->second.is_bot) return false;
    return found->second.is_bot ? IsLobbyHost(actor) : IsLobbyLeader(actor) && found->second.peer_id == owner->second.peer_id;
}

bool Online::RenameLobbyPlayer(PlayerID id, const std::string& name, PlayerID actor) {
    if (!CanRenameLobbyPlayer(id, actor) || !OnlineUtility::IsValidPlayerName(name)) return false;
    actor = ResolveLobbyActor(actor);
    auto found = online_session->player_states.find(id);
    Json::Value request; request["action"] = "name"; request["actor"] = actor; request["id"] = id; request["name"] = name;
    if (found != online_session->player_states.end()) {
        const auto index = found->second.local_index;
        if (!found->second.is_bot && found->second.peer_id == LocalPeerID() && index < local_lobby_profiles_.size()) local_lobby_profiles_[index].name = name;
    }
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::CanSetLobbyTeam(PlayerID id, PlayerID actor) const {
    if (!online_session) return false;
    const auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end() && lobby_slots_.count(id)) return LobbyWaiting() && IsLobbyHost(ResolveLobbyActor(actor));
    if (found != online_session->player_states.end() && found->second.is_bot)
        return !LobbyMigrationBusy() && IsActive() && IsLobbyHost(ResolveLobbyActor(actor));
    return !LobbyMigrationBusy() && IsActive() && lobby_authority_.CanSetTeam(LobbySeats(), ResolveLobbyActor(actor), id,
        lobby_host_controls_teams_, UsesLobbyTeams());
}

bool Online::SetLobbyTeam(PlayerID id, int team, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!online_session || !CanSetLobbyTeam(id, actor) || team < 1 || team > TeamChoiceLimit(lobby_slots_)) return false;
    Json::Value request; request["action"] = "team"; request["actor"] = actor; request["id"] = id; request["team"] = team;
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::SetLobbyScreen(PlayerID id, int screen, PlayerID actor) {
    if (LobbyMigrationBusy() || !IsActive() || !online_session) return false;
    actor = ResolveLobbyActor(actor);
    if (!IsLobbyLeader(actor)) return false;
    auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end() || found->second.is_bot || found->second.peer_id != LocalPeerID()) return false;
    const auto count = GetLocalPlayerStates().size();
    if (screen < 1 || static_cast<size_t>(screen) > count) return false;
    Json::Value request; request["action"] = "screen"; request["actor"] = actor; request["id"] = id; request["screen"] = screen;
    SubmitLobbyRequest(Encode(request)); return true;
}

void Online::ApplyLocalLobbyScreens() {
    if (!online_session) return;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    for (auto& entry : online_session->player_states) {
        PlayerState& player = entry.second;
        if (player.is_bot || player.peer_id != LocalPeerID()) continue;
        player.camera_id = std::max(0, player.screen - 1);
        if (player.local_index < local_lobby_profiles_.size()) local_lobby_profiles_[player.local_index].screen = player.screen;
        Object* object = graph ? graph->GetObjectFromID(player.object_id) : nullptr;
        if (object && object->GetType() == _movement_object)
            static_cast<MovementObject*>(object)->camera_id = player.camera_id;
    }
}

bool Online::CanRemoveLobbyPlayer(PlayerID id, PlayerID actor) const {
    if (online_session && !online_session->player_states.count(id) && lobby_slots_.count(id))
        return LobbyWaiting() && !LobbyMigrationBusy() && IsLobbyHost(ResolveLobbyActor(actor));
    if (online_session) {
        const auto found = online_session->player_states.find(id);
        if (found != online_session->player_states.end() && found->second.is_bot)
            return !LobbyMigrationBusy() && IsActive() && IsLobbyHost(ResolveLobbyActor(actor));
    }
    return !LobbyMigrationBusy() && IsActive() && lobby_waiting_ && online_session &&
        lobby_authority_.CanRemove(LobbySeats(), ResolveLobbyActor(actor), id);
}

void Online::RemoveLocalLobbyProfile(unsigned index) {
    if (local_lobby_profiles_.size() <= 1 || index >= local_lobby_profiles_.size()) return;
    LocalLobbyPlayerRemoving(index);
    const int screen = local_lobby_profiles_[index].screen;
    local_lobby_profiles_.erase(local_lobby_profiles_.begin() + index);
    for (auto& profile : local_lobby_profiles_) if (profile.screen > screen) --profile.screen;
    Input::Instance()->RemoveLocalPlayerInput(index);
}

bool Online::RemoveLobbyPlayer(PlayerID id, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (!CanRemoveLobbyPlayer(id, actor)) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "remove"; request["actor"] = actor; request["id"] = id;
        SubmitLobbyRequest(Encode(request));
        return true;
    }
    if (!online_session->player_states.count(id)) {
        lobby_slots_.erase(id);
        FriendlyProgression::ForgetPlayer(id);
        BroadcastLobby();
        return true;
    }
    const PlayerState removed = online_session->player_states.at(id);
    if (removed.is_bot) {
        const CommonObjectID common = GetOriginalID(removed.object_id);
        coop_cutscene_players.erase(removed.object_id);
        coop_hidden.erase(std::remove(coop_hidden.begin(), coop_hidden.end(), removed.object_id), coop_hidden.end());
        mode_teams_.erase(common);
        initialized_appearances.erase(id);
        SceneGraph* graph = Engine::Instance()->GetSceneGraph();
        Object* object = graph && removed.object_id != static_cast<ObjectID>(-1) ? graph->GetObjectFromID(removed.object_id) : nullptr;
        if (object) graph->map_editor->RemoveObject(object, graph);
        FriendlyProgression::ForgetPlayer(id);
        lobby_slots_.erase(id);
        online_session->player_states.erase(id);
        online_session->player_inputs.erase(id);
        Send<OnlineMessages::RemovePlayerState>(id);
        BroadcastLobby();
        return true;
    }
    const auto actor_seat = online_session->player_states.find(actor);
    if (actor_seat == online_session->player_states.end()) return false;
    if (removed.peer_id != actor_seat->second.peer_id && IsLobbyLeader(id)) {
        // Removing a remote client's designated leader kicks that context.
        // A former primary is an ordinary removable seat after promotion.
        // The reliable request dispatcher may already hold the peer lock.
        for (const auto& peer : online_session->connected_peers) if (peer.peer_id == removed.peer_id) {
            CloseConnection(peer.conn_id, ConnectionClosedReason::DISCONNECTED);
            return true;
        }
        return false;
    }
    ReleaseMenuPause(id);
    if (removed.peer_id == LocalPeerID()) RemoveLocalLobbyProfile(removed.local_index);
    if (removed.peer_id != LocalPeerID()) {
        if (PlayerInput* input = Input::Instance()->GetController(removed.controller_id)) {
            input->enabled = false; input->key_down.clear();
        }
        ActiveCameras::Instance()->FreeVirtualCameraInstance(removed.camera_id);
    }
    // The human leaves their persistent unit available in its original slot.
    online_session->player_states.erase(id);
    online_session->player_inputs.erase(id);
    for (auto& entry : online_session->player_states) if (!entry.second.is_bot && entry.second.peer_id == removed.peer_id) {
        auto& player = entry.second;
        if (player.local_index > removed.local_index) --player.local_index;
        if (player.screen > removed.screen) --player.screen;
        if (player.peer_id == LocalPeerID()) {
            player.controller_id = Input::Instance()->GetLocalPlayerInputID(player.local_index);
            player.camera_id = player.screen - 1;
        }
    }
    BroadcastLobby();
    return true;
}

void Online::PrepareReturnToLobby() {
    host_started_level = false;
    lobby_waiting_ = true; lobby_start_pending_ = false;
    menu_pausers_.clear();
    loading = false; time_frozen_ = false; build_session_ = false; build_on_load_ = false;
    preloaded_level.clear(); preload_loaded = false;
    level_name.clear(); campaign_id.clear();
    own_character_applied.clear();
    initialized_appearances.clear(); appearance_graph = nullptr; appearance_level.clear();
    natural_player_team.clear(); natural_team_level.clear(); natural_team_graph = nullptr;
    ResetCampaignFactions();
    held_hits.clear(); pending_revives.clear(); pending_avatar_characters.clear();
    pending_unsticks.clear(); pending_drop_orders.clear(); pending_item_launches.clear();
    owner_overrides.clear(); handed_back_ms.clear();
    stream_latest.clear(); stream_sent.clear(); stream_owned.clear(); stream_next_capture_ms = 0.0;
    client_player_key_states_.clear();
    coop_hidden.clear(); last_safe_ground.clear(); coop_cutscene = false;
    mode_teams_.clear();
    campaign_main_avatar_id_ = static_cast<ObjectID>(-1);
    remote_cutscene_active_ = false;
    online_session->free_avatars.clear(); online_session->taken_avatars.clear();
    online_session->player_inputs.clear();
    while (!online_session->peer_queued_level_updates.empty()) online_session->peer_queued_level_updates.pop();
    while (!online_session->peer_queued_sync_updates.empty()) online_session->peer_queued_sync_updates.pop();
    {
        lock_guard<mutex> guard(online_session->state_packages_lock);
        online_session->state_packages.clear();
    }
    script_api_outgoing.clear(); script_api_deliveries.clear();
    for (auto& player : online_session->player_states) player.second.object_id = -1;
    {
        lock_guard<mutex> guard(online_session->outgoing_messages_lock);
        auto is_level = [](const OnlineMessageRef& ref) {
            const auto category = static_cast<OnlineMessageBase*>(ref.GetData())->cat;
            return category == OnlineMessageCategory::LEVEL_PERSISTENT || category == OnlineMessageCategory::LEVEL_TRANSIENT;
        };
        auto& outgoing = online_session->outgoing_messages;
        outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(), [&is_level](const OnlineSession::OutgoingMessage& m) {
            return is_level(m.message);
        }), outgoing.end());
        auto& persistent = online_session->persistent_outgoing_messages;
        persistent.erase(std::remove_if(persistent.begin(), persistent.end(), is_level), persistent.end());
    }
    Engine::Instance()->ReturnToLobby();
}

bool Online::EndLobbyGame(PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || lobby_waiting_ || !online_session) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "end_game"; request["actor"] = actor;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    PrepareReturnToLobby();
    online_session->client_connection_manager.ParkConnectionsInLobby();
    BroadcastLobby();
    return true;
}

vec3 Online::LobbyTeamColor(int team) {
    static const vec3 colors[] = {
        vec3(0.20f, 0.48f, 0.90f), vec3(0.85f, 0.23f, 0.20f), vec3(0.23f, 0.68f, 0.34f),
        vec3(0.92f, 0.71f, 0.16f), vec3(0.62f, 0.30f, 0.82f), vec3(0.14f, 0.70f, 0.72f),
        vec3(0.94f, 0.43f, 0.17f), vec3(0.88f, 0.37f, 0.66f)
    };
    if (team <= 8) return colors[std::max(1, team) - 1];
    const float hue = std::fmod(static_cast<float>(team - 9) * 0.61803398875f, 1.0f) * 6.0f;
    const int sector = static_cast<int>(hue);
    const float low = 0.22f, high = 0.90f;
    const float down = high - (high - low) * (hue - sector);
    const float up = low + (high - low) * (hue - sector);
    switch (sector) {
        case 0: return vec3(high, up, low); case 1: return vec3(down, high, low);
        case 2: return vec3(low, high, up); case 3: return vec3(low, down, high);
        case 4: return vec3(up, low, high); default: return vec3(high, low, down);
    }
}

void Online::ApplyLobbyAppearances(SceneGraph* graph) {
    if (!graph || !host_started_level || lobby_waiting_) return;
    if (appearance_graph != graph || appearance_level != level_name || own_character_applied.empty()) {
        initialized_appearances.clear(); appearance_graph = graph; appearance_level = level_name;
        own_character_applied = "lobby:" + level_name;
    }
    const std::string turner = "Data/Characters/turner.xml";
    auto apply_palette = [this](MovementObject* avatar, int team, bool primary, bool arena_assigned) {
        OGPalette palette(*avatar->GetPalette());
        bool changed = false;
        for (auto& channel : palette) {
            std::string label = channel.label;
            std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            vec3 color = channel.color;
            if (label.find("fur") != std::string::npos) color = primary ? vec3(0.55f, 0.42f, 0.30f) : LobbyTeamColor(team);
            else if (label.find("cloth") != std::string::npos)
                color = primary && !arena_assigned ? vec3(0.40f, 0.12f, 0.10f) : LobbyTeamColor(team) * 0.55f;
            if (distance_squared(color, channel.color) > 0.000001f) { channel.color = color; changed = true; }
        }
        if (changed) avatar->ApplyPalette(palette, !IsLocalAvatar(avatar->GetID()));
    };
    std::set<ObjectID> humans;
    for (const auto& player : GetPlayerStates()) {
        Object* object = graph->GetObjectFromID(player.object_id);
        if (!object || object->GetType() != _movement_object) continue;
        auto* avatar = static_cast<MovementObject*>(object);
        humans.insert(player.object_id);
        const bool preserve_authored = IsLocalOnlySession() && player.player_id == campaign_main_player_id_;
        const bool first_assignment = initialized_appearances.find(player.player_id) == initialized_appearances.end() ||
            initialized_appearances[player.player_id] != player.object_id;
        if (!player.is_bot && !preserve_authored && first_assignment && avatar->character_path != turner) {
            if (IsLocalAvatar(player.object_id) && avatar->HasFunction("void SwitchCharacter(string)"))
                avatar->Execute("SwitchCharacter(\"" + turner + "\");");
            else avatar->ApplyNetworkCharacter(turner);
        }
        initialized_appearances[player.player_id] = player.object_id;
        const bool primary = IsLobbyHost(player.player_id);
        const int assigned = AssignedModeTeam(player.object_id);
        const auto mode_assignment = mode_teams_.find(GetOriginalID(player.object_id));
        const bool arena_assignment = mode_assignment != mode_teams_.end() && mode_assignment->second > 0;
        ApplyAssignedTeam(player.object_id);
        if (preserve_authored && assigned == 0 && !UsesLobbyTeams()) continue;
        apply_palette(avatar, assigned > 0 ? assigned : UsesLobbyTeams() ? player.team : 1, primary, arena_assignment);
    }
    for (const auto& assignment : mode_teams_) {
        const ObjectID id = GetObjectID(assignment.first);
        if (humans.count(id)) continue;
        Object* object = graph->GetObjectFromID(id);
        if (!object || object->GetType() != _movement_object) continue;
        ApplyAssignedTeam(id);
        apply_palette(static_cast<MovementObject*>(object), assignment.second, false, true);
    }
}

bool Online::SetLobbySelection(const std::string& mode, const std::string& campaign, const std::string& map, bool build, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || !lobby_waiting_ || mode.size() > 256 || campaign.size() > 256 || map.size() > 4096) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "selection"; request["actor"] = actor;
        request["mode"] = mode; request["campaign"] = campaign; request["map"] = map; request["build"] = build;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    lobby_mode_ = mode; lobby_campaign_ = campaign; lobby_map_ = map; lobby_build_ = build;
    BroadcastLobby(); return true;
}

bool Online::SetLobbyAllowAllPause(bool allow, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || !lobby_waiting_) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "allow_pause"; request["actor"] = actor; request["value"] = allow;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    lobby_allow_all_pause_ = allow; BroadcastLobby(); return true;
}

bool Online::SetLobbyForceTeams(bool force, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || !lobby_waiting_) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "force_teams"; request["actor"] = actor; request["value"] = force;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    lobby_force_teams_ = force; BroadcastLobby(); return true;
}

bool Online::SetLobbyHostControlsTeams(bool controls, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || !IsActive()) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "host_controls_teams"; request["actor"] = actor; request["value"] = controls;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    lobby_host_controls_teams_ = controls; BroadcastLobby(); return true;
}

bool Online::SetCampaignMatchSetting(const std::string& key, const std::string& value, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    std::string normalized;
    if (!IsActive() || LobbyMigrationBusy() || !IsLobbyHost(actor) ||
        !MatchSettingAvailable(key) || !NormalizeCampaignMatchSetting(key, value, &normalized)) return false;
    if (IsClient()) {
        Json::Value request;
        request["action"] = "match_setting"; request["actor"] = actor;
        request["key"] = key; request["value"] = normalized;
        SubmitLobbyRequest(Encode(request));
        return true;
    }
    const Config before = config;
    if (key == "difficulty_preset") config.SetDifficultyPreset(normalized);
    else config.GetRef(key) = normalized;
    if (config != before) {
        config.ReloadDynamicSettings();
        BroadcastLobby();
    }
    return true;
}

void Online::ApplyManualTeam(PlayerID id) {
    if (lobby_waiting_ || !online_session) return;
    const auto player = online_session->player_states.find(id);
    if (player == online_session->player_states.end()) return;
    const ObjectID avatar = player->second.object_id;
    if (avatar == static_cast<ObjectID>(-1)) return;
    const auto assigned = mode_teams_.find(GetOriginalID(avatar));
    if (assigned != mode_teams_.end())
        assigned->second = assigned->second > 0 ? player->second.team : -player->second.team;
    else if (!UsesLobbyTeams())
        // An explicit host edit overrides this fighter for the current match.
        // Keep map overrides distinct from Arena's authored team assignment;
        // the usual world cleanup clears both before the next match.
        mode_teams_[GetOriginalID(avatar)] = -player->second.team;
    ApplyAssignedTeam(avatar);
}

bool Online::UsesLobbyTeams() const {
    return lobby_force_teams_ || lobby_build_ || lobby_mode_ == "freeplay" ||
        lobby_mode_.find("sandbox") != std::string::npos || lobby_campaign_.find("sandbox") != std::string::npos ||
        (lobby_mode_.empty() && lobby_campaign_.empty() && campaign_id.empty());
}

int Online::AssignedModeTeam(ObjectID avatar) const {
    const auto found = mode_teams_.find(GetOriginalID(avatar));
    return found == mode_teams_.end() ? 0 : static_cast<int>(std::abs(static_cast<int64_t>(found->second)));
}

void Online::ApplyAssignedTeam(ObjectID avatar) {
    const auto assignment = mode_teams_.find(GetOriginalID(avatar));
    const int assigned = assignment == mode_teams_.end() ? 0 : assignment->second;
    std::string team;
    const PlayerState* roster = nullptr;
    if (online_session) for (const auto& player : online_session->player_states)
        if (player.second.object_id == avatar) { roster = &player.second; break; }
    if (roster && CoopCampaign()) {
        SceneGraph* team_scene = Engine::Instance()->GetSceneGraph();
        RefreshCampaignFactions(natural_player_team, natural_team_graph == team_scene && natural_team_level == level_name);
        if (lobby_force_teams_) team = CampaignTeamTags(roster->team);
        else {
            // Ignoring lobby teams preserves the mode's current story faction,
            // including scripted allegiance changes, across the whole party.
            SceneGraph* scene = Engine::Instance()->GetSceneGraph();
            Object* main = scene ? scene->GetObjectFromID(CampaignMainAvatarID()) : nullptr;
            if (main && main->GetType() == _movement_object && main->GetScriptParams()->HasParam("Teams"))
                team = main->GetScriptParams()->GetStringVal("Teams");
            if (team.empty() || team.find(CampaignFactionPrefix()) != std::string::npos)
                team = CampaignFactionMainTags().empty() ? natural_player_team : CampaignFactionMainTags();
            if (team.empty()) team = CampaignTeamTags(CampaignMainLobbyTeam());
        }
    } else if (assigned > 0) team = "arena_team_" + std::to_string(assigned);
    else if (assigned < 0) team = assigned < -1 ? "lobby_team_" + std::to_string(-assigned) :
        natural_player_team.empty() ? "lobby_team_1" : natural_player_team;
    else if (UsesLobbyTeams() && roster)
        team = roster->team > 1 ? "lobby_team_" + std::to_string(roster->team) :
            natural_player_team.empty() ? "lobby_team_1" : natural_player_team;
    if (team.empty()) return;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    Object* object = graph ? graph->GetObjectFromID(avatar) : nullptr;
    if (!object || object->GetType() != _movement_object) return;
    if (object->GetScriptParams()->HasParam("Teams") && object->GetScriptParams()->GetStringVal("Teams") == team) return;
    ScriptParam param; param.SetString(team);
    object->GetScriptParams()->InsertScriptParam("Teams", param);
    static_cast<MovementObject*>(object)->UpdateScriptParams();
}

bool Online::AssignModeTeams(const std::vector<std::pair<ObjectID, int>>& assignments) {
    if (!IsHosting() || lobby_waiting_ || !online_session) return false;
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (!graph) return false;
    std::unordered_map<CommonObjectID, int> next;
    for (const auto& assignment : assignments) {
        Object* object = graph->GetObjectFromID(assignment.first);
        if (!object || object->GetType() != _movement_object || assignment.second < 1) return false;
        int team = assignment.second;
        if (lobby_force_teams_) for (const auto& player : online_session->player_states)
            if (player.second.object_id == assignment.first) { team = player.second.team; break; }
        next[GetOriginalID(assignment.first)] = team;
    }
    mode_teams_.swap(next);
    for (const auto& assignment : assignments) ApplyAssignedTeam(assignment.first);
    BroadcastLobby();
    return true;
}

bool Online::RequestPause(unsigned local_index, bool paused, bool from_menu) {
    if (!IsActive()) return false;
    const auto states = GetLocalPlayerStates();
    auto player = std::find_if(states.begin(), states.end(), [local_index](const PlayerState& p) { return p.local_index == local_index; });
    if (player == states.end() || (!lobby_allow_all_pause_ && !IsLobbyHost(player->player_id))) return false;
    Json::Value request; request["action"] = from_menu ? "menu_pause" : "pause"; request["id"] = player->player_id; request["paused"] = paused;
    SubmitLobbyRequest(Encode(request)); return true;
}

Json::Value Online::SaveGameRules() const {
    Json::Value rules;
    rules["match"] = Json::Value(Json::objectValue);
    for (const char* key : migration_match_settings)
        if (MatchSettingAvailable(key) && config.HasKey(key)) rules["match"][key] = config[key].str();
    if (!rules["match"].isMember("oo_arena_random")) rules["match"]["oo_arena_random"] = "0";
    if (!rules["match"].isMember("oo_public_game")) rules["match"]["oo_public_game"] = "1";
    rules["preset"] = config.GetDifficultyPreset();
    rules["join_character"] = default_hot_join_characte_path;
    rules["allow_pause"] = lobby_allow_all_pause_;
    rules["force_teams"] = lobby_force_teams_;
    rules["host_teams"] = lobby_host_controls_teams_;
    rules["build"] = lobby_build_;
    rules["mode_settings"] = LobbyModeSettings();
    return rules;
}

bool Online::ValidateGameRules(const Json::Value& rules) {
    if (!rules.isObject() || !rules["match"].isObject() || !rules["preset"].isString() ||
        !rules["allow_pause"].isBool() || !rules["force_teams"].isBool() || !rules["host_teams"].isBool() ||
        !rules["build"].isBool() || !ReceiveLobbyModeSettings(rules["mode_settings"], true)) return false;
    if (rules.isMember("join_character") && (!rules["join_character"].isString() ||
        !OnlineUtility::IsSafeCharacterPath(rules["join_character"].asString()))) return false;
    for (const auto& key : rules["match"].getMemberNames()) {
        const auto& entry = rules["match"][key];
        if (!entry.isString() || entry.asString().size() > 64) return false;
        const auto value = entry.asString();
        if (key == "oo_arena_bots") {
            if (value.empty() || value.size() > 4 || value.find_first_not_of("0123456789") != std::string::npos) return false;
        } else if (key == "oo_public_game") {
            if (value != "0" && value != "1" && value != "true" && value != "false") return false;
        } else {
            std::string normalized;
            if (!NormalizeCampaignMatchSetting(key, value, &normalized)) return false;
        }
    }
    return true;
}

void Online::RestoreGameRules(const Json::Value& rules) {
    if (rules["join_character"].isString()) SetDefaultHotJoinCharacter(rules["join_character"].asString());
    std::string preset;
    if (NormalizeCampaignMatchSetting("difficulty_preset", rules["preset"].asString(), &preset))
        config.SetDifficultyPreset(preset);
    for (const char* key : migration_match_settings) {
        const auto& value = rules["match"][key];
        if (MatchSettingAvailable(key) && value.isString()) config.GetRef(key) = value.asString();
    }
    config.ReloadDynamicSettings();
    lobby_allow_all_pause_ = rules["allow_pause"].asBool();
    lobby_force_teams_ = rules["force_teams"].asBool();
    lobby_host_controls_teams_ = rules["host_teams"].asBool();
    ReceiveLobbyModeSettings(rules["mode_settings"]);
}

bool Online::StartLobbyGame(const std::string& level, const std::string& campaign, bool build, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    if (LobbyMigrationBusy() || !IsLobbyHost(actor) || !lobby_waiting_) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "start"; request["actor"] = actor;
        SubmitLobbyRequest(Encode(request)); return true;
    }
    const std::string selected = level.empty() ? lobby_map_ : level;
    const std::string camp = campaign.empty() ? lobby_campaign_ : campaign;
    if (selected.empty() || selected.find("..") != std::string::npos) return false;
    // Every connected peer must have completed its roster handshake first.
    for (const auto& peer : online_session->connected_peers) {
        bool present = false;
        for (const auto& p : online_session->player_states) present = present || (!p.second.is_bot && p.second.peer_id == peer.peer_id);
        if (!present) return false;
    }
    lobby_waiting_ = false;
    mode_teams_.clear();
    const bool building = level.empty() ? lobby_build_ : build;
    if (!SessionSaves::ResumeLobby(lobby_mode_, camp, selected, building)) ResetModeChallenge();
    lobby_start_pending_ = true;
    if (building) BuildOnLoad();
    SetBuildSession(building);
    BroadcastLobby();
    std::string level_id;
    if (!camp.empty()) {
        Engine::Instance()->ScriptableUICallback("set_campaign " + camp);
        std::string lookup = camp;
        const auto authored = ModLoading::Instance().GetCampaign(lookup);
        const std::string relative = selected.compare(0, 12, "Data/Levels/") == 0 ? selected.substr(12) : selected;
        for (const auto& entry : authored.levels) {
            const std::string path = entry.path.str();
            if (path == relative || path == selected || entry.id.str() == selected) { level_id = entry.id.str(); break; }
        }
    }
    Engine::Instance()->ScriptableUICallback(level_id.empty() ? selected : "load_campaign_level " + level_id);
    return true;
}

void Online::ReceiveLobbyControl(PeerID from, const std::string& payload) {
    if (!online_session) return;
    Json::Value message; Json::Reader reader;
    if (!reader.parse(payload, message, false) || !message.isObject()) return;
    if (HandleLobbyMigrationControl(from, payload)) return;
    const std::string action = message.get("action", "").asString();
    if (LobbyMigrationBusy() && action != "snapshot" && action != "device") return;
    if (IsClient()) {
        if (from == 0 && action == "join_denied") {
            const unsigned index = message.get("local_index", UINT32_MAX).asUInt();
            if (index < local_lobby_profiles_.size() && index >= GetLocalPlayerStates().size()) RemoveLocalLobbyProfile(index);
            return;
        }
        if (from == 0 && action == "mod_state") {
            SessionMods::ReceiveUpdate(message["session_mods"]);
            return;
        }
        if (from != 0 || action != "snapshot" || !message["players"].isArray()) return;
        std::string mod_error;
        if (!ReceiveLobbyModeSettings(message["mode_settings"], true) ||
            !SessionMods::Validate(message["session_mods"], SessionMods::Scope::Network, mod_error)) return;
        const bool returning = !lobby_waiting_ && message.get("waiting", false).asBool();
        if (returning) PrepareReturnToLobby();
        std::set<PlayerID> incoming_local;
        for (const auto& row : message["players"]) if (!row.get("bot", false).asBool() && row.get("peer", 0).asUInt() == LocalPeerID())
            incoming_local.insert(row.get("id", 0).asUInt());
        std::vector<unsigned> removed_local;
        const auto previous_local = GetLocalPlayerStates();
        std::set<PlayerID> previous_ids;
        for (const auto& previous : previous_local) previous_ids.insert(previous.player_id);
        for (const auto& previous : previous_local) if (!incoming_local.count(previous.player_id)) {
            bool rebound = false;
            for (const auto& row : message["players"]) if (!row.get("bot", false).asBool() && row.get("peer", 0).asUInt() == LocalPeerID() &&
                row.get("local_index", UINT32_MAX).asUInt() == previous.local_index && !previous_ids.count(row.get("id", 0).asUInt())) rebound = true;
            if (!rebound) removed_local.push_back(previous.local_index);
        }
        std::sort(removed_local.rbegin(), removed_local.rend());
        for (unsigned index : removed_local) RemoveLocalLobbyProfile(index);
        lobby_waiting_ = message.get("waiting", false).asBool();
        lobby_mode_ = message.get("mode", "").asString(); lobby_campaign_ = message.get("campaign", "").asString();
        lobby_map_ = message.get("map", "").asString(); lobby_build_ = message.get("build", false).asBool();
        lobby_allow_all_pause_ = message.get("allow_all_pause", false).asBool();
        lobby_force_teams_ = message.get("force_teams", false).asBool();
        lobby_host_controls_teams_ = message.get("host_controls_teams", true).asBool();
        std::map<PlayerID, LobbySlot> definitions;
        PlayerID next_slot;
        if (DecodeSlotDefinitions(message["slot_definitions"], definitions, next_slot)) {
            lobby_slots_ = std::move(definitions);
            lobby_auto_admission_ = message["slot_definitions"]["auto_admit"].asBool();
            next_local_player_id_ = next_slot;
        }
        ReceiveLobbyModeSettings(message["mode_settings"]);
        SessionMods::Restore(message["session_mods"], SessionMods::Scope::Network);
        ApplyCampaignMatchSnapshot(message["match_settings"]);
        lobby_authority_.host = message.get("decision_host", 255).asUInt();
        remote_cutscene_active_ = message.get("cutscene", false).asBool();
        campaign_main_player_id_ = message.get("campaign_main_player", 255).asUInt();
        campaign_main_avatar_id_ = loading ? static_cast<ObjectID>(-1) : GetObjectID(message.get("campaign_main_object", -1).asInt());
        lobby_authority_.leaders.clear();
        for (const auto& leader : message["leaders"])
            lobby_authority_.leaders[static_cast<PeerID>(leader.get("peer", 255).asUInt())] = leader.get("player", 255).asUInt();
        natural_player_team = message.get("natural_team", "").asString();
        const auto& faction_data = message["campaign_factions"];
        if (faction_data.isObject() && faction_data["level"].isString() && faction_data["main_tags"].isString() &&
            faction_data["prefix"].isString() && faction_data["enemies"].isArray()) {
            std::vector<std::string> profiles;
            for (const auto& profile : faction_data["enemies"]) if (profile.isString()) profiles.push_back(profile.asString());
            ReceiveCampaignFactions(faction_data["level"].asString(), faction_data["main_tags"].asString(), profiles,
                                    faction_data["prefix"].asString(), faction_data.get("main_team", 1).asInt(),
                                    faction_data.get("anchor_player", static_cast<Json::UInt>(campaign_main_player_id_)).asUInt());
        }
        mode_teams_.clear();
        for (const auto& assignment : message["mode_teams"]) {
            const int team = assignment.get("team", 0).asInt();
            if (team != 0 && team != std::numeric_limits<int>::min()) mode_teams_[assignment.get("object", -1).asInt()] = team;
        }
        time_frozen_ = message.get("frozen", false).asBool();
        Engine::Instance()->CommitPause();
        std::set<PlayerID> present;
        for (const auto& row : message["players"]) {
            PlayerState state;
            state.player_id = row.get("id", 0).asUInt(); state.peer_id = static_cast<PeerID>(row.get("peer", 0).asUInt());
            state.local_index = row.get("local_index", 0).asUInt(); state.playername = row.get("name", "UNKNOWN").asString();
            state.team = row.get("team", 1).asInt(); state.screen = row.get("screen", 1).asInt(); state.connected = row.get("connected", true).asBool();
            state.is_bot = row.get("bot", false).asBool();
            state.object_id = loading ? static_cast<ObjectID>(-1) : GetObjectID(row.get("object", -1).asInt());
            state.controller_id = row.get("controller", -1).asInt(); state.camera_id = row.get("camera", -1).asInt();
            if (state.is_bot) {
                state.peer_id = 255; state.local_index = std::numeric_limits<uint32_t>::max();
                state.screen = 0; state.controller_id = -1; state.camera_id = -1;
                SceneGraph* graph = Engine::Instance()->GetSceneGraph();
                Object* object = graph && state.object_id != static_cast<ObjectID>(-1) ? graph->GetObjectFromID(state.object_id) : nullptr;
                if (object && object->GetType() == _movement_object) {
                    auto* bot = static_cast<MovementObject*>(object);
                    bot->is_player = false; bot->controlled = false; bot->controller_id = -1;
                    bot->camera_id = 0;  // NPC eye queries reuse the existing main camera; no view is allocated.
                }
            }
            if (!state.is_bot && state.peer_id == LocalPeerID() && state.local_index < local_lobby_profiles_.size()) {
                local_lobby_profiles_[state.local_index].name = state.playername;
                local_lobby_profiles_[state.local_index].screen = state.screen;
            }
            present.insert(state.player_id);
            online_session->player_states[state.player_id] = state;
        }
        for (auto it = online_session->player_states.begin(); it != online_session->player_states.end();) {
            if (!present.count(it->first)) it = online_session->player_states.erase(it); else ++it;
        }
        Input::Instance()->SetUpForXPlayers(LocalLobbyPlayerCount(), true);
        ApplyLocalLobbyScreens();
        // A device can join while the initial handshake is in flight. Submit
        // the next unacknowledged seat after each authoritative roster update.
        {
            size_t acknowledged = 0;
            for (const auto& entry : online_session->player_states) if (!entry.second.is_bot && entry.second.peer_id == LocalPeerID()) ++acknowledged;
            if (acknowledged > 0 && acknowledged < local_lobby_profiles_.size()) {
                Json::Value request;
                request["action"] = "join"; request["local_index"] = static_cast<Json::UInt>(acknowledged);
                request["name"] = local_lobby_profiles_[acknowledged].name;
                SubmitLobbyRequest(Encode(request));
            }
        }
        return;
    }
    if (!IsHosting()) return;
    if (action == "device") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto player = online_session->player_states.find(id);
        if (player == online_session->player_states.end() || player->second.is_bot || player->second.peer_id != from) return;
        const bool connected = message.get("connected", false).asBool();
        if (player->second.connected != connected) {
            player->second.connected = connected;
            BroadcastLobby();
        }
        return;
    }
    const PlayerID actor = message.get("actor", static_cast<Json::UInt>(LobbyAuthority::InvalidPlayer)).asUInt();
    const auto acting_player = online_session->player_states.find(actor);
    // A reliable control message may act only for a logical player owned by
    // its sending machine. No role grant or fabricated ID bypasses this gate.
    const bool valid_actor = acting_player != online_session->player_states.end() && !acting_player->second.is_bot && acting_player->second.peer_id == from;
    if (action == "slot_admission") {
        if (valid_actor && message["automatic"].isBool()) SetLobbyAutoAdmission(message["automatic"].asBool(), actor);
        return;
    }
    if (action == "slot_kind") {
        if (valid_actor && message["id"].isUInt() && message["player"].isBool()) SetLobbySlotPlayer(message["id"].asUInt(), message["player"].asBool(), actor);
        return;
    }
    if (action == "slot_swap") {
        if (valid_actor && message["first"].isUInt() && message["second"].isUInt()) SwapLobbySlots(message["first"].asUInt(), message["second"].asUInt(), actor);
        return;
    }
    if (action == "progress_hit" || action == "progress_recover") { FriendlyProgression::ReceiveEvent(from, message); return; }
    if (action == "progress_action") {
        if (valid_actor) FriendlyProgression::ReceiveAction(actor, message);
        return;
    }
    if (action == "match_setting") {
        if (valid_actor && message["key"].isString() && message["value"].isString())
            SetCampaignMatchSetting(message["key"].asString(), message["value"].asString(), actor);
        return;
    }
    if (action == "pause" || action == "menu_pause") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto player = online_session->player_states.find(id);
        if (player == online_session->player_states.end() || player->second.is_bot || player->second.peer_id != from ||
            (!lobby_allow_all_pause_ && !IsLobbyHost(id))) return;
        const bool pause = message.get("paused", false).asBool();
        if (action == "menu_pause") {
            if (lobby_waiting_ || !host_started_level) return;
            if (pause) {
                // A menu may join another shortcut pause. Explicit freezes
                // belong to the host and survive the closing of every menu.
                if (!time_frozen_ || !menu_pausers_.empty()) {
                    if (!time_frozen_) SetTimeFrozen(true);
                    menu_pausers_.insert(id);
                }
            } else {
                ReleaseMenuPause(id);
            }
        } else {
            SetTimeFrozen(pause);
        }
        BroadcastLobby();
        return;
    }
    if (action == "team") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        const int team = message.get("team", 0).asInt();
        if (!valid_actor || !CanSetLobbyTeam(id, actor) || team < 1 || team > TeamChoiceLimit(lobby_slots_)) return;
        lobby_slots_.at(id).team = team;
        if (online_session->player_states.count(id)) {
            online_session->player_states.at(id).team = team;
            ApplyManualTeam(id);
        }
        BroadcastLobby(); return;
    }
    if (action == "screen") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto found = online_session->player_states.find(id);
        if (!valid_actor || !IsLobbyLeader(actor) || found == online_session->player_states.end() || found->second.is_bot || found->second.peer_id != from) return;
        const int screen = message.get("screen", 0).asInt();
        size_t count = 0;
        for (const auto& seat : online_session->player_states) if (!seat.second.is_bot && seat.second.peer_id == from) ++count;
        if (screen < 1 || static_cast<size_t>(screen) > count) return;
        const int old = found->second.screen;
        for (auto& seat : online_session->player_states)
            if (!seat.second.is_bot && seat.second.peer_id == from && seat.first != id && seat.second.screen == screen) seat.second.screen = old;
        found->second.screen = screen;
        if (from == LocalPeerID()) ApplyLocalLobbyScreens();
        BroadcastLobby(); return;
    }
    if (action == "host_controls_teams") {
        if (valid_actor) SetLobbyHostControlsTeams(message.get("value", true).asBool(), actor);
        return;
    }
    if (action == "end_game") {
        if (valid_actor) EndLobbyGame(actor);
        return;
    }
    if (action == "name") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto found = online_session->player_states.find(id);
        const std::string name = message.get("name", "").asString();
        if (!valid_actor || !CanRenameLobbyPlayer(id, actor) ||
            !OnlineUtility::IsValidPlayerName(name)) return;
        if (found == online_session->player_states.end()) {
            lobby_slots_.at(id).name = name;
            BroadcastLobby(); return;
        }
        found->second.playername = name;
        if (found->second.is_bot && found->second.object_id != static_cast<ObjectID>(-1)) {
            SceneGraph* graph = Engine::Instance()->GetSceneGraph();
            Object* object = graph ? graph->GetObjectFromID(found->second.object_id) : nullptr;
            if (object) {
                object->GetScriptParams()->ASSetString("Bot Name", name);
                if (object->GetType() == _movement_object) static_cast<MovementObject*>(object)->UpdateScriptParams();
            }
        }
        BroadcastLobby(); return;
    }
    if (action == "join") {
        bool admitted = false;
        std::vector<PlayerState> owned;
        for (const auto& seat : online_session->player_states) if (!seat.second.is_bot && seat.second.peer_id == from) {
            admitted = true; owned.push_back(seat.second);
        }
        if (!admitted) return;
        const unsigned index = message.get("local_index", 0).asUInt();
        if (index != owned.size()) return;
        std::sort(owned.begin(), owned.end(), [](const PlayerState& a, const PlayerState& b) { return a.local_index < b.local_index; });
        std::vector<std::string> names;
        for (const auto& seat : owned) names.push_back(seat.playername);
        names.push_back(NameOrDefault(message.get("name", "").asString(), index));
        if (!EstablishPeerPlayers(from, names)) {
            Json::Value denied; denied["action"] = "join_denied"; denied["local_index"] = index;
            const Peer* peer = GetPeerFromID(from);
            if (peer) SendTo<OnlineMessages::LobbyControl>(peer->conn_id, Encode(denied));
            return;
        }
        if (!loading && (from == LocalPeerID() || online_session->client_connection_manager.IsClientLoaded(from))) AssignPeerAvatars(from);
        BroadcastLobby(); return;
    }
    if (action == "remove") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        if (valid_actor) RemoveLobbyPlayer(id, actor);
        return;
    }
    if (!lobby_waiting_) return;
    if (action == "mode_setting") {
        if (valid_actor && message["value"].isInt() && message["key"].isString())
            SetModeSetting(message.get("key", "").asString(), message["value"].asInt(), actor);
        return;
    }
    if (action == "promote_host") {
        const PlayerID target = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        if (!valid_actor || !CanPromoteLobbyHost(target, actor)) return;
        if (online_session->player_states.at(target).peer_id != LocalPeerID()) {
            BeginLobbyHostMigration(target, actor);
        } else {
            lobby_authority_.host = target;
            BroadcastLobby();
        }
        return;
    }
    if (action == "promote_leader") {
        const PlayerID target = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        if (!valid_actor || !CanPromoteLobbyLeader(target, actor)) return;
        lobby_authority_.leaders[from] = target;
        BroadcastLobby(); return;
    }
    if (action == "selection" || action == "allow_pause" || action == "force_teams" || action == "start") {
        if (!valid_actor || !IsLobbyHost(actor)) return;
        if (action == "selection") SetLobbySelection(message.get("mode", "").asString(), message.get("campaign", "").asString(),
            message.get("map", "").asString(), message.get("build", false).asBool(), actor);
        else if (action == "allow_pause") SetLobbyAllowAllPause(message.get("value", false).asBool(), actor);
        else if (action == "force_teams") SetLobbyForceTeams(message.get("value", false).asBool(), actor);
        else StartLobbyGame("", "", false, actor);
        return;
    }
}
