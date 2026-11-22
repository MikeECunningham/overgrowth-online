// Session roster and pre-game lobby control. Apache-2.0.
#include <Online/online.h>
#include <Online/online_utility.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <JSON/json.h>
#include <Scripting/angelscript/lobby_bindings.h>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace {
std::string Encode(const Json::Value& value) { return Json::FastWriter().write(value); }
std::string NameOrDefault(const std::string& name, unsigned index) {
    if (OnlineUtility::IsValidPlayerName(name)) return name;
    return index == 0 ? "Unknown Rabbit" : "Local player " + std::to_string(index + 1);
}
std::map<PlayerID, ObjectID> initialized_appearances;
const SceneGraph* appearance_graph = nullptr;
std::string appearance_level;
std::string natural_player_team;
std::string natural_team_level;
const SceneGraph* natural_team_graph = nullptr;
}

PeerID Online::LocalPeerID() const {
    return IsHosting() || online_session == nullptr ? 255 : static_cast<PeerID>(online_session->local_player_id);
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

unsigned Online::LocalLobbyPlayerCount() const {
    if (IsActive() && !lobby_waiting_ && online_session != nullptr) {
        unsigned count = 0;
        for (const auto& entry : online_session->player_states) if (entry.second.peer_id == LocalPeerID()) ++count;
        return std::max(1u, count);
    }
    return static_cast<unsigned>(LocalLobbyNames().size());
}

std::vector<PlayerState> Online::GetLocalPlayerStates() const {
    std::vector<PlayerState> result;
    if (online_session != nullptr) {
        for (const auto& entry : online_session->player_states) {
            if (entry.second.peer_id == LocalPeerID()) {
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

bool Online::BeginLobby() {
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
    mode_teams_.clear();
    level_name.clear(); campaign_id.clear();
    StartHostingMultiplayer();
    EstablishPeerPlayers(255, LocalLobbyNames());
    return IsHosting();
}

PlayerID Online::AddLocalLobbyPlayer(const std::string& name) {
    if (IsActive() && !lobby_waiting_) return static_cast<PlayerID>(-1);
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

void Online::EstablishPeerPlayers(PeerID peer, const std::vector<std::string>& names, const std::vector<int>& requested_screens) {
    if (!IsHosting() || online_session == nullptr) return;
    const size_t count = std::max<size_t>(1, names.size());
    std::vector<int> screens = peer == LocalPeerID() ? LocalLobbyScreens() : requested_screens;
    std::set<int> unique_screens(screens.begin(), screens.end());
    const bool valid_screens = screens.size() == count && unique_screens.size() == count &&
        *unique_screens.begin() == 1 && *unique_screens.rbegin() == static_cast<int>(count);
    for (size_t index = 0; index < count; ++index) {
        auto found = std::find_if(online_session->player_states.begin(), online_session->player_states.end(),
            [peer, index](const std::pair<const PlayerID, PlayerState>& p) { return p.second.peer_id == peer && p.second.local_index == index; });
        if (found != online_session->player_states.end()) continue;
        const PlayerID id = index == 0 ? peer : next_local_player_id_++;
        PlayerState state;
        state.player_id = id;
        state.peer_id = peer;
        state.local_index = static_cast<uint32_t>(index);
        state.playername = NameOrDefault(index < names.size() ? names[index] : "", static_cast<unsigned>(index));
        state.object_id = -1;
        state.screen = valid_screens ? screens[index] : static_cast<int>(index + 1);
        state.team = lobby_mode_ == "com-wolfire-arena" || lobby_campaign_ == "com-wolfire-arena" ?
            static_cast<int>(online_session->player_states.size() + 1) : 1;
        online_session->player_states[id] = state;
        if (peer == LocalPeerID()) {
            online_session->player_states[id].controller_id = Input::Instance()->GetLocalPlayerInputID(static_cast<unsigned>(index));
            online_session->player_states[id].camera_id = state.screen - 1;
        } else {
            AssignNewControllerForPlayer(id);
        }
    }
    BroadcastLobby();
}

void Online::AssignPeerAvatars(PeerID peer) {
    if (!IsHosting() || lobby_waiting_ || !Engine::Instance()->IsLevelLoaded()) return;
    std::vector<PlayerID> ids;
    for (const auto& entry : online_session->player_states) if (entry.second.peer_id == peer) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end(), [this](PlayerID a, PlayerID b) {
        return online_session->player_states[a].local_index < online_session->player_states[b].local_index;
    });
    for (PlayerID id : ids) {
        auto& state = online_session->player_states[id];
        if (state.object_id != static_cast<ObjectID>(-1)) {
            RemoveFreeAvatarId(state.object_id);
            continue;
        }
        ObjectID avatar = GetFreeAvatarID();
        if (avatar == static_cast<ObjectID>(-1)) avatar = CreateCharacter();
        if (avatar == static_cast<ObjectID>(-1)) continue;
        SceneGraph* graph = Engine::Instance()->GetSceneGraph();
        Object* object = graph->GetObjectFromID(avatar);
        if (natural_team_graph != graph || natural_team_level != level_name) {
            natural_player_team.clear(); natural_team_graph = graph; natural_team_level = level_name;
        }
        if (natural_player_team.empty() && object && object->GetScriptParams()->HasParam("Teams"))
            natural_player_team = object->GetScriptParams()->GetStringVal("Teams");
        PossessAvatar(id, avatar);
        ApplyAssignedTeam(avatar);
    }
}

std::string Online::LobbyPayload() const {
    Json::Value message;
    message["action"] = "snapshot";
    message["waiting"] = lobby_waiting_;
    message["mode"] = lobby_mode_; message["campaign"] = lobby_campaign_; message["map"] = lobby_map_;
    message["build"] = lobby_build_; message["allow_all_pause"] = lobby_allow_all_pause_;
    message["force_teams"] = lobby_force_teams_;
    message["natural_team"] = natural_player_team;
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
        row["object"] = static_cast<int>(GetOriginalID(p.object_id));
        row["controller"] = p.controller_id; row["camera"] = p.camera_id;
        message["players"].append(row);
    }
    return Encode(message);
}

void Online::BroadcastLobby() {
    if (!IsHosting() || online_session == nullptr) return;
    if (lobby_waiting_) {
        std::set<int> teams;
        bool outside = false;
        for (const auto& entry : online_session->player_states) {
            teams.insert(entry.second.team);
            outside = outside || entry.second.team < 1 || static_cast<size_t>(entry.second.team) > online_session->player_states.size();
        }
        if (outside) {
            std::map<int, int> compact;
            int next = 1;
            for (int team : teams) compact[team] = next++;
            for (auto& entry : online_session->player_states) entry.second.team = compact[entry.second.team];
        }
    }
    Send<OnlineMessages::LobbyControl>(LobbyPayload());
}

void Online::SubmitLobbyRequest(const std::string& payload) {
    if (IsHosting()) ReceiveLobbyControl(255, payload);
    else if (IsClient()) Send<OnlineMessages::LobbyControl>(payload);
}

bool Online::RenameLobbyPlayer(PlayerID id, const std::string& name) {
    if (!lobby_waiting_ || !online_session || !OnlineUtility::IsValidPlayerName(name)) return false;
    auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end() || found->second.peer_id != LocalPeerID()) return false;
    Json::Value request; request["action"] = "name"; request["id"] = id; request["name"] = name;
    const auto index = found->second.local_index;
    if (index < local_lobby_profiles_.size()) local_lobby_profiles_[index].name = name;
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::SetLobbyTeam(PlayerID id, int team) {
    if (!IsHosting() || !lobby_waiting_ || !online_session || team < 1 || static_cast<size_t>(team) > online_session->player_states.size()) return false;
    auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end()) return false;
    found->second.team = team; BroadcastLobby(); return true;
}

bool Online::SetLobbyScreen(PlayerID id, int screen) {
    if (!lobby_waiting_ || !online_session) return false;
    auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end() || found->second.peer_id != LocalPeerID()) return false;
    const auto count = GetLocalPlayerStates().size();
    if (screen < 1 || static_cast<size_t>(screen) > count) return false;
    Json::Value request; request["action"] = "screen"; request["id"] = id; request["screen"] = screen;
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::CanRemoveLobbyPlayer(PlayerID id) const {
    if (!IsActive() || !lobby_waiting_ || !online_session || id == 255) return false;
    const auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end()) return false;
    return found->second.peer_id == LocalPeerID() ? found->second.local_index > 0 : IsHosting();
}

void Online::RemoveLocalLobbyProfile(unsigned index) {
    if (index == 0 || index >= local_lobby_profiles_.size()) return;
    LocalLobbyPlayerRemoving(index);
    const int screen = local_lobby_profiles_[index].screen;
    local_lobby_profiles_.erase(local_lobby_profiles_.begin() + index);
    for (auto& profile : local_lobby_profiles_) if (profile.screen > screen) --profile.screen;
    Input::Instance()->RemoveLocalPlayerInput(index);
}

bool Online::RemoveLobbyPlayer(PlayerID id) {
    if (!CanRemoveLobbyPlayer(id)) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "remove"; request["id"] = id;
        SubmitLobbyRequest(Encode(request));
        return true;
    }
    const PlayerState removed = online_session->player_states.at(id);
    if (removed.local_index == 0) {
        // A peer's primary represents its connection: kicking it removes all
        // its local seats together. Extra seats can be removed individually.
        for (const auto& peer : GetPeers()) if (peer.peer_id == removed.peer_id) {
            CloseConnection(peer.conn_id, ConnectionClosedReason::DISCONNECTED);
            return true;
        }
        return false;
    }
    if (removed.peer_id == LocalPeerID()) RemoveLocalLobbyProfile(removed.local_index);
    if (removed.peer_id != LocalPeerID()) {
        if (PlayerInput* input = Input::Instance()->GetController(removed.controller_id)) {
            input->enabled = false; input->key_down.clear();
        }
        ActiveCameras::Instance()->FreeVirtualCameraInstance(removed.camera_id);
    }
    online_session->player_states.erase(id);
    online_session->player_inputs.erase(id);
    for (auto& entry : online_session->player_states) if (entry.second.peer_id == removed.peer_id) {
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
    loading = false; time_frozen_ = false; build_session_ = false; build_on_load_ = false;
    preloaded_level.clear(); preload_loaded = false;
    level_name.clear(); campaign_id.clear();
    own_character_applied.clear();
    initialized_appearances.clear(); appearance_graph = nullptr; appearance_level.clear();
    natural_player_team.clear(); natural_team_level.clear(); natural_team_graph = nullptr;
    held_hits.clear(); pending_revives.clear(); pending_avatar_characters.clear();
    pending_unsticks.clear(); pending_drop_orders.clear(); pending_item_launches.clear();
    owner_overrides.clear(); handed_back_ms.clear();
    stream_latest.clear(); stream_sent.clear(); stream_owned.clear(); stream_next_capture_ms = 0.0;
    client_player_key_states_.clear();
    coop_hidden.clear(); last_safe_ground.clear(); coop_cutscene = false;
    mode_teams_.clear();
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

bool Online::EndLobbyGame() {
    if (!IsHosting() || lobby_waiting_ || !online_session) return false;
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
        const bool first_assignment = initialized_appearances.find(player.player_id) == initialized_appearances.end() ||
            initialized_appearances[player.player_id] != player.object_id;
        if (first_assignment && avatar->character_path != turner) {
            if (IsLocalAvatar(player.object_id) && avatar->HasFunction("void SwitchCharacter(string)"))
                avatar->Execute("SwitchCharacter(\"" + turner + "\");");
            else avatar->ApplyNetworkCharacter(turner);
        }
        initialized_appearances[player.player_id] = player.object_id;
        const bool primary = player.peer_id == 255 && player.local_index == 0;
        const int assigned = AssignedModeTeam(player.object_id);
        ApplyAssignedTeam(player.object_id);
        apply_palette(avatar, assigned > 0 ? assigned : UsesLobbyTeams() ? player.team : 1, primary, assigned > 0);
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

bool Online::SetLobbySelection(const std::string& mode, const std::string& campaign, const std::string& map, bool build) {
    if (!IsHosting() || !lobby_waiting_ || mode.size() > 256 || campaign.size() > 256 || map.size() > 4096) return false;
    lobby_mode_ = mode; lobby_campaign_ = campaign; lobby_map_ = map; lobby_build_ = build;
    BroadcastLobby(); return true;
}

bool Online::SetLobbyAllowAllPause(bool allow) {
    if (!IsHosting() || !lobby_waiting_) return false;
    lobby_allow_all_pause_ = allow; BroadcastLobby(); return true;
}

bool Online::SetLobbyForceTeams(bool force) {
    if (!IsHosting() || !lobby_waiting_) return false;
    lobby_force_teams_ = force; BroadcastLobby(); return true;
}

bool Online::UsesLobbyTeams() const {
    return lobby_force_teams_ || lobby_build_ ||
        lobby_mode_.find("sandbox") != std::string::npos || lobby_campaign_.find("sandbox") != std::string::npos ||
        (lobby_mode_.empty() && lobby_campaign_.empty() && campaign_id.empty());
}

int Online::AssignedModeTeam(ObjectID avatar) const {
    const auto found = mode_teams_.find(GetOriginalID(avatar));
    return found == mode_teams_.end() ? 0 : found->second;
}

void Online::ApplyAssignedTeam(ObjectID avatar) {
    const int assigned = AssignedModeTeam(avatar);
    std::string team;
    if (assigned > 0) team = "arena_team_" + std::to_string(assigned);
    else if (UsesLobbyTeams() && online_session) {
        for (const auto& player : online_session->player_states) if (player.second.object_id == avatar) {
            team = player.second.team > 1 ? "lobby_team_" + std::to_string(player.second.team) : natural_player_team;
            break;
        }
    }
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

bool Online::RequestPause(unsigned local_index, bool paused) {
    if (!IsActive()) return false;
    const auto states = GetLocalPlayerStates();
    auto player = std::find_if(states.begin(), states.end(), [local_index](const PlayerState& p) { return p.local_index == local_index; });
    if (player == states.end() || (!lobby_allow_all_pause_ && !(IsHosting() && local_index == 0))) return false;
    Json::Value request; request["action"] = "pause"; request["id"] = player->player_id; request["paused"] = paused;
    SubmitLobbyRequest(Encode(request)); return true;
}

bool Online::StartLobbyGame(const std::string& level, const std::string& campaign, bool build) {
    if (!IsHosting() || !lobby_waiting_) return false;
    const std::string selected = level.empty() ? lobby_map_ : level;
    const std::string camp = campaign.empty() ? lobby_campaign_ : campaign;
    if (selected.empty() || selected.find("..") != std::string::npos) return false;
    // Every connected peer must have completed its roster handshake first.
    for (const auto& peer : online_session->connected_peers) {
        bool present = false;
        for (const auto& p : online_session->player_states) present = present || p.second.peer_id == peer.peer_id;
        if (!present) return false;
    }
    lobby_waiting_ = false;
    mode_teams_.clear();
    lobby_start_pending_ = true;
    const bool building = level.empty() ? lobby_build_ : build;
    if (building) BuildOnLoad();
    SetBuildSession(building);
    BroadcastLobby();
    if (!camp.empty()) Engine::Instance()->ScriptableUICallback("set_campaign " + camp);
    Engine::Instance()->ScriptableUICallback(selected);
    return true;
}

void Online::ReceiveLobbyControl(PeerID from, const std::string& payload) {
    if (!online_session) return;
    Json::Value message; Json::Reader reader;
    if (!reader.parse(payload, message, false) || !message.isObject()) return;
    const std::string action = message.get("action", "").asString();
    if (IsClient()) {
        if (from != 0 || action != "snapshot" || !message["players"].isArray()) return;
        const bool returning = !lobby_waiting_ && message.get("waiting", false).asBool();
        if (returning) PrepareReturnToLobby();
        std::set<PlayerID> incoming_local;
        for (const auto& row : message["players"]) if (row.get("peer", 0).asUInt() == LocalPeerID())
            incoming_local.insert(row.get("id", 0).asUInt());
        std::vector<unsigned> removed_local;
        for (const auto& previous : GetLocalPlayerStates()) if (previous.local_index > 0 && !incoming_local.count(previous.player_id))
            removed_local.push_back(previous.local_index);
        std::sort(removed_local.rbegin(), removed_local.rend());
        for (unsigned index : removed_local) RemoveLocalLobbyProfile(index);
        lobby_waiting_ = message.get("waiting", false).asBool();
        lobby_mode_ = message.get("mode", "").asString(); lobby_campaign_ = message.get("campaign", "").asString();
        lobby_map_ = message.get("map", "").asString(); lobby_build_ = message.get("build", false).asBool();
        lobby_allow_all_pause_ = message.get("allow_all_pause", false).asBool();
        lobby_force_teams_ = message.get("force_teams", false).asBool();
        natural_player_team = message.get("natural_team", "").asString();
        mode_teams_.clear();
        for (const auto& assignment : message["mode_teams"]) {
            const int team = assignment.get("team", 0).asInt();
            if (team > 0) mode_teams_[assignment.get("object", -1).asInt()] = team;
        }
        time_frozen_ = message.get("frozen", false).asBool();
        Engine::Instance()->CommitPause();
        std::set<PlayerID> present;
        for (const auto& row : message["players"]) {
            PlayerState state;
            state.player_id = row.get("id", 0).asUInt(); state.peer_id = static_cast<PeerID>(row.get("peer", 0).asUInt());
            state.local_index = row.get("local_index", 0).asUInt(); state.playername = row.get("name", "UNKNOWN").asString();
            state.team = row.get("team", 1).asInt(); state.screen = row.get("screen", 1).asInt(); state.connected = row.get("connected", true).asBool();
            state.object_id = GetObjectID(row.get("object", -1).asInt());
            state.controller_id = row.get("controller", -1).asInt(); state.camera_id = row.get("camera", -1).asInt();
            if (state.peer_id == LocalPeerID() && state.local_index < local_lobby_profiles_.size()) {
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
        // A device can join while the initial handshake is in flight. Submit
        // the next unacknowledged seat after each authoritative roster update.
        if (lobby_waiting_) {
            size_t acknowledged = 0;
            for (const auto& entry : online_session->player_states) if (entry.second.peer_id == LocalPeerID()) ++acknowledged;
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
        if (player == online_session->player_states.end() || player->second.peer_id != from) return;
        const bool connected = message.get("connected", false).asBool();
        if (player->second.connected != connected) {
            player->second.connected = connected;
            BroadcastLobby();
        }
        return;
    }
    if (action == "pause") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto player = online_session->player_states.find(id);
        if (player == online_session->player_states.end() || player->second.peer_id != from ||
            (!lobby_allow_all_pause_ && !(from == 255 && player->second.local_index == 0))) return;
        SetTimeFrozen(message.get("paused", false).asBool());
        BroadcastLobby();
        return;
    }
    if (!lobby_waiting_) return;
    if (action == "remove") {
        const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
        auto player = online_session->player_states.find(id);
        if (player != online_session->player_states.end() && player->second.peer_id == from && player->second.local_index > 0)
            RemoveLobbyPlayer(id);
        return;
    }
    if (action == "join") {
        // Only peers whose version/mod handshake established a primary player
        // may append local units. A raw transport connection is not admission.
        auto primary = online_session->player_states.find(from);
        if (primary == online_session->player_states.end() || primary->second.peer_id != from) return;
        const unsigned index = message.get("local_index", 0).asUInt();
        std::vector<std::string> names;
        for (const auto& p : online_session->player_states) if (p.second.peer_id == from) names.push_back(p.second.playername);
        if (index != names.size()) return;  // append only; a forged index cannot claim somebody else's slot
        names.push_back(NameOrDefault(message.get("name", "").asString(), index));
        EstablishPeerPlayers(from, names);
        return;
    }
    const PlayerID id = message.get("id", static_cast<Json::UInt>(-1)).asUInt();
    auto found = online_session->player_states.find(id);
    if (found == online_session->player_states.end() || found->second.peer_id != from) return;
    if (action == "name") {
        const std::string name = message.get("name", "").asString();
        if (!OnlineUtility::IsValidPlayerName(name)) return;
        found->second.playername = name;
    } else if (action == "screen") {
        const int screen = message.get("screen", 0).asInt();
        size_t count = 0;
        for (const auto& p : online_session->player_states) if (p.second.peer_id == from) ++count;
        if (screen < 1 || static_cast<size_t>(screen) > count) return;
        const int old = found->second.screen;
        for (auto& p : online_session->player_states) if (p.second.peer_id == from && p.first != id && p.second.screen == screen) p.second.screen = old;
        found->second.screen = screen;
        if (from == LocalPeerID()) for (const auto& p : online_session->player_states) {
            if (p.second.peer_id == from && p.second.local_index < local_lobby_profiles_.size()) local_lobby_profiles_[p.second.local_index].screen = p.second.screen;
        }
    } else return;
    BroadcastLobby();
}
