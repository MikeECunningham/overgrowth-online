// Local lobby forms and device ownership. Apache-2.0.
#include "lobby_bindings.h"
#include <Scripting/angelscript/ascontext.h>
#include <Scripting/angelscript/add_on/scriptarray/scriptarray.h>
#include <Online/online.h>
#include <Online/online_utility.h>
#include <Main/engine.h>
#include <GUI/player_menu_context.h>
#include <Internal/config.h>
#include <UserInput/input.h>
#include <JSON/json.h>
#include <SDL.h>
#include <imgui.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <cstdlib>

namespace {
struct LobbyPlayer {
    unsigned id = 0;
    std::string name;
    int local_index = 0;
    bool local = false;
    bool host = false;
    bool leader = false;
    bool bot = false;
    int team = 1;
    int lobby_team = 1;
    int screen = 0;
    bool connected = true;
};
void ConstructPlayer(void* memory) { new (memory) LobbyPlayer(); }
void CopyPlayer(const LobbyPlayer& source, void* memory) { new (memory) LobbyPlayer(source); }
void DestroyPlayer(void* memory) { static_cast<LobbyPlayer*>(memory)->~LobbyPlayer(); }

unsigned MenuLocalPlayer();
unsigned MenuPlayerID() { return Online::Instance()->LobbyPlayerIDForLocalIndex(MenuLocalPlayer()); }
int CurrentLocalPlayerIndex() { return static_cast<int>(MenuLocalPlayer()); }
int LeaderInputID() {
    Online* online = Online::Instance();
    const PlayerID leader = online->LocalLobbyLeaderPlayerID();
    for (const auto& player : online->GetLocalPlayerStates())
        if (player.player_id == leader) return Input::Instance()->GetLocalPlayerInputID(player.local_index);
    return Input::Instance()->GetLocalPlayerInputID(0);
}
bool CanManageLocal() { return Online::Instance()->IsLobbyLeader(MenuPlayerID()); }
bool Active() {
    Online* online = Online::Instance();
    return online->IsActive() && online->LobbyWaiting() &&
        (!online->IsClient() || online->LobbyPlayerIDForLocalIndex(0) != LobbyAuthority::InvalidPlayer);
}
bool LocalOnly() { return Online::Instance()->IsLocalOnlySession(); }
bool OpenToNetwork() { return Online::Instance()->OpenLobbyToNetwork(MenuPlayerID()); }
bool IsHost() { return Online::Instance()->IsLobbyHost(MenuPlayerID()); }
unsigned LocalCount() { return std::max(1u, Input::Instance()->GetNumLocalPlayers()); }
int LocalIndex(int actual) { return Input::Instance()->LocalPlayerIndexForInputID(actual); }
bool BeginHost() {
    if (!CanManageLocal() || !Online::Instance()->BeginLobby()) return false;
    config.GetRef("controller_menu_owner") = 0;
    return true;
}
bool AddLocal() {
    if (!CanManageLocal()) return false;
    return Online::Instance()->AddLocalLobbyPlayer("") != static_cast<PlayerID>(-1);
}
bool AddBot() { return Online::Instance()->AddLobbyBot("", MenuPlayerID()) != LobbyAuthority::InvalidPlayer; }
bool CanRename(unsigned id) { return Online::Instance()->CanRenameLobbyPlayer(id, MenuPlayerID()); }
bool CanStart() {
    Online* online = Online::Instance();
    if (!IsHost() || !online->LobbyWaiting() || online->LobbyMap().empty() || online->LobbyMigrationBusy()) return false;
    const auto players = online->GetPlayerStates();
    for (const auto& peer : online->GetPeers()) {
        bool found = false;
        for (const auto& player : players) found = found || player.peer_id == peer.peer_id;
        if (!found) return false;
    }
    return true;
}
bool Start() { return CanStart() && Online::Instance()->StartLobbyGame("", "", false, MenuPlayerID()); }
bool CanLeave() {
    Online* online = Online::Instance();
    // Admission has not assigned a leader yet. The person trying to connect
    // must still be able to cancel instead of waiting for transport timeout.
    return !online->IsActive() || IsHost() || CanManageLocal() ||
        (online->IsClient() && online->LobbyPlayerIDForLocalIndex(0) == LobbyAuthority::InvalidPlayer);
}
bool TryLeave() {
    if (!CanLeave()) return false;
    // Explicit Leave must also cancel a handoff while it is between sessions.
    Online::Instance()->QueueStopMultiplayer();
    return true;
}
void Leave() { TryLeave(); }
std::string Mode() { return Online::Instance()->LobbyMode(); }
std::string Campaign() { return Online::Instance()->LobbyCampaign(); }
std::string Map() { return Online::Instance()->LobbyMap(); }
bool Build() { return Online::Instance()->LobbyBuild(); }
bool AllowAllPause() { return Online::Instance()->LobbyAllowAllPause(); }
bool ForceTeams() { return Online::Instance()->LobbyForceTeams(); }
bool SetForceTeams(bool force) { return Online::Instance()->SetLobbyForceTeams(force, MenuPlayerID()); }
bool HostControlsTeams() { return Online::Instance()->LobbyHostControlsTeams(); }
bool SetHostControlsTeams(bool controls) { return Online::Instance()->SetLobbyHostControlsTeams(controls, MenuPlayerID()); }
bool CanSetTeam(unsigned id) { return Online::Instance()->CanSetLobbyTeam(id, MenuPlayerID()); }
bool CanPromoteHost(unsigned id) { return Online::Instance()->CanPromoteLobbyHost(id, MenuPlayerID()); }
bool PromoteHost(unsigned id) { return Online::Instance()->PromoteLobbyHost(id, MenuPlayerID()); }
bool CanPromoteLeader(unsigned id) { return Online::Instance()->CanPromoteLobbyLeader(id, MenuPlayerID()); }
bool PromoteLeader(unsigned id) { return Online::Instance()->PromoteLobbyLeader(id, MenuPlayerID()); }
std::string MigrationStatus() { return Online::Instance()->LobbyMigrationStatus(); }
bool MigrationBusy() { return Online::Instance()->LobbyMigrationBusy(); }
int AssignedTeam(int avatar) { return Online::Instance()->AssignedModeTeam(avatar); }
bool AssignTeams(const CScriptArray& avatars, const CScriptArray& teams) {
    if (!Online::Instance()->IsHosting() || avatars.GetSize() != teams.GetSize()) return false;
    std::vector<std::pair<ObjectID, int>> assignments;
    for (asUINT i = 0; i < avatars.GetSize(); ++i)
        assignments.emplace_back(*static_cast<const int*>(avatars.At(i)), *static_cast<const int*>(teams.At(i)));
    return Online::Instance()->AssignModeTeams(assignments);
}
bool SetSelection(const std::string& mode, const std::string& campaign, const std::string& map, bool build) {
    return Online::Instance()->SetLobbySelection(mode, campaign, map, build, MenuPlayerID());
}
bool SetName(unsigned id, const std::string& name) {
    if (!Online::Instance()->RenameLobbyPlayer(id, name, MenuPlayerID())) return false;
    for (const auto& p : Online::Instance()->GetLocalPlayerStates()) {
        if (p.player_id == id && p.local_index == 0) config.GetRef("playername") = name;
    }
    return true;
}
bool SetTeam(unsigned id, int team) { return Online::Instance()->SetLobbyTeam(id, team, MenuPlayerID()); }
bool SetScreen(unsigned id, int screen) { return CanManageLocal() && Online::Instance()->SetLobbyScreen(id, screen, MenuPlayerID()); }
bool SetAllowAllPause(bool allowed) { return Online::Instance()->SetLobbyAllowAllPause(allowed, MenuPlayerID()); }
bool CanRemove(unsigned id) { return Online::Instance()->CanRemoveLobbyPlayer(id, MenuPlayerID()); }
bool Remove(unsigned id) { return CanRemove(id) && Online::Instance()->RemoveLobbyPlayer(id, MenuPlayerID()); }
bool EndGame() { return Online::Instance()->EndLobbyGame(MenuPlayerID()); }
bool join_input_enabled = true;
void SetJoinInputEnabled(bool enabled) { join_input_enabled = enabled; }

unsigned MenuLocalPlayer() {
    if (const auto* menu = PlayerMenuUI::Current()) return static_cast<unsigned>(std::max(0, menu->local_index));
    Engine* engine = Engine::Instance();
    if (engine->current_engine_state_.type != kEngineScriptableUIState && engine->current_menu_player >= 0) {
        const int logical = Input::Instance()->LocalPlayerIndexForInputID(engine->current_menu_player);
        if (logical >= 0) return static_cast<unsigned>(logical);
    }
    // controller.as stores the logical owner, unlike the engine's actual
    // input ID. These differ after removing a seat or allocating remote slots.
    const int count = std::max(1u, Input::Instance()->GetNumLocalPlayers());
    return static_cast<unsigned>(std::max(0, std::min(count - 1, config["controller_menu_owner"].toNumber<int>())));
}
int CurrentInputID() { return Input::Instance()->GetLocalPlayerInputID(MenuLocalPlayer()); }
bool CanPause() {
    const unsigned local = MenuLocalPlayer();
    Online* online = Online::Instance();
    return online->IsActive() ? (online->LobbyAllowAllPause() || online->IsLobbyHost(online->LobbyPlayerIDForLocalIndex(local))) : local == 0;
}
bool SetPaused(bool paused) {
    if (!CanPause()) return false;
    if (Online::Instance()->IsActive()) return Online::Instance()->RequestPause(MenuLocalPlayer(), paused);
    Engine::Instance()->user_paused = paused;
    Engine::Instance()->CommitPause();
    return true;
}

std::vector<PlayerState> Players() {
    auto players = Online::Instance()->IsActive() ? Online::Instance()->GetPlayerStates() : Online::Instance()->GetLocalPlayerStates();
    std::sort(players.begin(), players.end(), [](const PlayerState& a, const PlayerState& b) {
        if (a.is_bot != b.is_bot) return !a.is_bot;
        if (a.is_bot) return a.player_id < b.player_id;
        if (a.peer_id != b.peer_id) return a.peer_id == 255 || (b.peer_id != 255 && a.peer_id < b.peer_id);
        return a.local_index < b.local_index;
    });
    return players;
}
CScriptArray* PlayerArray() {
    asIScriptEngine* engine = asGetActiveContext()->GetEngine();
    CScriptArray* array = CScriptArray::Create(engine->GetTypeInfoByDecl("array<LobbyPlayer>"), static_cast<asUINT>(0));
    for (const auto& player : Players()) {
        LobbyPlayer row;
        row.id = player.player_id; row.name = player.playername; row.local_index = player.is_bot ? -1 : static_cast<int>(player.local_index);
        row.bot = player.is_bot;
        row.local = !player.is_bot && player.peer_id == Online::Instance()->LocalPeerID();
        row.host = !player.is_bot && Online::Instance()->IsLobbyHost(player.player_id);
        row.leader = !player.is_bot && Online::Instance()->IsLobbyLeader(player.player_id);
        row.lobby_team = player.team;
        const int assigned = Online::Instance()->AssignedModeTeam(player.object_id);
        row.team = !Online::Instance()->LobbyWaiting() ? (assigned > 0 ? assigned : Online::Instance()->UsesLobbyTeams() ? player.team : 1) : player.team;
        row.screen = row.local ? player.screen : 0; row.connected = player.connected;
        array->InsertLast(&row);
    }
    return array;
}
unsigned Revision() {
    std::ostringstream state;
    state << Mode() << '|' << Campaign() << '|' << Map() << '|' << Build() << '|' << AllowAllPause() << '|' << ForceTeams()
          << '|' << HostControlsTeams() << '|' << MigrationStatus() << '|' << MigrationBusy();
    for (const auto& player : Players()) {
        state << '|' << player.player_id << ':' << player.peer_id << ':' << player.local_index << ':' << player.playername
              << ':' << player.team << ':' << player.screen << ':' << player.connected << ':' << Online::Instance()->IsLobbyHost(player.player_id)
              << ':' << Online::Instance()->IsLobbyLeader(player.player_id) << ':' << Online::Instance()->AssignedModeTeam(player.object_id) << ':' << player.is_bot;
    }
    return static_cast<unsigned>(std::hash<std::string>()(state.str()));
}

std::string DeviceIdentity(SDL_Joystick* joystick) {
    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
    std::string key = guid;
    const char* name = SDL_JoystickName(joystick);
    if (name) key += std::string("|") + name;
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char* serial = SDL_JoystickGetSerial(joystick);
    if (serial && *serial) return key + "|serial:" + serial;
#endif
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const char* path = SDL_JoystickPath(joystick);
    if (path && *path) {
#if defined(__linux__)
        // /dev/input/eventN changes on a reconnect. The USB topology behind it
        // stays stable; discard the kernel's input/inputNN instance suffix.
        const std::string device(path);
        const size_t slash = device.find_last_of('/');
        if (device.compare(0, 11, "/dev/input/") == 0 && slash != std::string::npos) {
            const std::string sysfs = "/sys/class/input/" + device.substr(slash + 1) + "/device";
            char* resolved = realpath(sysfs.c_str(), nullptr);
            if (resolved) {
                std::string physical(resolved); free(resolved);
                const size_t input_node = physical.find("/input/input");
                if (input_node != std::string::npos) physical.resize(input_node);
                // HID's .000N device-instance suffix also changes on replug.
                const size_t usb_hid = physical.rfind("/0003:");
                if (usb_hid != std::string::npos) physical.resize(usb_hid);
                key += "|port:" + physical;
            }
        }
        // If the platform has no stable identity, GUID/name plus an unclaimed
        // remembered slot still restores a lone reconnected pad reliably.
#else
        key += std::string("|path:") + path;
#endif
    }
#endif
    return key;
}
struct JoinedDevice { std::string identity; int local = -1; };
std::map<int, JoinedDevice> joined_devices;
std::map<std::string, std::vector<int>> remembered_slots;
std::map<int, bool> held_buttons;
std::set<int> joining_buttons_held;
struct DeviceProfile { std::string name; std::map<std::string, std::string> settings; };
std::map<std::string, DeviceProfile> removed_device_profiles;
bool was_lobby = false;
bool was_world = false;
int world_primary_device = -1;
int previous_button_instance = -1;
float previous_button_time = 0.0f;
bool used_lobby_context = false;
void NotifyDevice(unsigned local_index, bool connected) {
    Online* online = Online::Instance();
    if (!online->IsActive()) return;
    for (const auto& player : online->GetLocalPlayerStates()) {
        if (player.local_index != local_index || player.connected == connected) continue;
        Json::Value request;
        request["action"] = "device"; request["id"] = player.player_id; request["connected"] = connected;
        const std::string payload = Json::FastWriter().write(request);
        if (online->IsHosting()) online->ReceiveLobbyControl(255, payload);
        else online->Send<OnlineMessages::LobbyControl>(payload);
    }
}
DeviceProfile SaveDeviceProfile(unsigned index) {
    DeviceProfile profile;
    for (const auto& player : Online::Instance()->GetLocalPlayerStates()) if (player.local_index == index) profile.name = player.playername;
    const std::string prefix = "gamepad_" + std::to_string(index);
    for (const auto& entry : config.map_) {
        if (entry.first.compare(0, prefix.size(), prefix) == 0 && entry.first.size() > prefix.size() &&
            (entry.first[prefix.size()] == '_' || entry.first[prefix.size()] == '['))
            profile.settings[entry.first.substr(prefix.size())] = entry.second.data.str();
    }
    return profile;
}
void RestoreDeviceProfile(const std::string& identity, unsigned index) {
    const auto saved = removed_device_profiles.find(identity);
    if (saved == removed_device_profiles.end()) return;
    const std::string prefix = "gamepad_" + std::to_string(index);
    for (auto it = config.map_.begin(); it != config.map_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0 && it->first.size() > prefix.size() &&
            (it->first[prefix.size()] == '_' || it->first[prefix.size()] == '[')) it = config.map_.erase(it);
        else ++it;
    }
    for (const auto& setting : saved->second.settings) config.GetRef(prefix + setting.first) = setting.second;
    Input::Instance()->SetFromConfig(config);
    Input::Instance()->UpdateGamepadLookSensitivity();
    Input::Instance()->UpdateGamepadDeadzone();
}
}

void LocalLobbyPlayerRemoving(unsigned index) {
    const DeviceProfile profile = SaveDeviceProfile(index);
    for (auto& memory : remembered_slots) {
        bool removed = false;
        for (auto it = memory.second.begin(); it != memory.second.end();) {
            if (*it == static_cast<int>(index)) { removed = true; it = memory.second.erase(it); }
            else { if (*it > static_cast<int>(index)) --*it; ++it; }
        }
        if (removed) removed_device_profiles[memory.first] = profile;
    }
    for (auto it = joined_devices.begin(); it != joined_devices.end();) {
        if (it->second.local == static_cast<int>(index)) {
            removed_device_profiles[it->second.identity] = profile;
            Input::Instance()->BindLocalJoystick(it->first, -1);
            joining_buttons_held.erase(it->first);
            it = joined_devices.erase(it);
        } else { if (it->second.local > static_cast<int>(index)) --it->second.local; ++it; }
    }
}

void UpdateLocalLobbyInput() {
    Online* online = Online::Instance();
    Input* input = Input::Instance();
    const bool lobby = online->IsActive() && online->LobbyWaiting();
    Engine* engine = Engine::Instance();
    const bool world = engine->IsLevelLoaded() || engine->current_engine_state_.type == kEngineLevelState ||
        engine->current_engine_state_.type == kEngineEditorLevelState;
    if (world && !was_world) {
        world_primary_device = previous_button_time > input->last_keyboard_event_time &&
            previous_button_time > input->last_mouse_event_time ? previous_button_instance : -1;
    }
    was_world = world;
    previous_button_instance = input->LastControllerButtonInstance();
    previous_button_time = input->LastControllerButtonTime();
    const bool keyboard_join = input->KeyboardJoinPressed();  // consume stale menu presses too
    if (!online->IsActive() && !world) { was_lobby = false; return; }
    const unsigned local_count = online->IsActive() ? online->LocalLobbyPlayerCount() : std::max(1u, input->GetNumLocalPlayers());
    auto& joysticks = input->GetOpenJoysticks();
    // A pad remembered from an earlier offline/local context can already be
    // P1 when it opens Host. It still takes primary gameplay ownership here.
    if (!was_lobby && lobby && input->LocalKeyboardPlayerIndex() == 0 &&
        input->LastControllerButtonTime() > input->last_keyboard_event_time &&
        input->LastControllerButtonTime() > input->last_mouse_event_time) {
        const auto primary = joined_devices.find(input->LastControllerButtonInstance());
        if (primary != joined_devices.end() && primary->second.local == 0) input->BindLocalKeyboard(-1);
    }
    used_lobby_context = used_lobby_context || lobby;
    std::set<int> present, occupied;
    for (const auto& entry : joysticks) present.insert(entry.first);
    for (auto it = joined_devices.begin(); it != joined_devices.end();) {
        if (!present.count(it->first) || it->second.local < 0 || static_cast<unsigned>(it->second.local) >= local_count) {
            if (!present.count(it->first) && it->second.local >= 0) NotifyDevice(static_cast<unsigned>(it->second.local), false);
            held_buttons.erase(it->first);
            joining_buttons_held.erase(it->first);
            it = joined_devices.erase(it);
        }
        else { occupied.insert(it->second.local); ++it; }
    }
    for (auto& entry : joysticks) {
        SDL_Joystick* joystick = entry.second->sdl_joystick;
        if (!joystick || !SDL_JoystickGetAttached(joystick)) continue;
        const int instance = entry.first;
        bool down = false;
        for (int button = 0; button < SDL_JoystickNumButtons(joystick); ++button) down = down || SDL_JoystickGetButton(joystick, button) != 0;
        const bool rising = down && !held_buttons[instance];
        held_buttons[instance] = down;
        auto joined = joined_devices.find(instance);
        if (joined == joined_devices.end()) {
            const std::string identity = DeviceIdentity(joystick);
            int slot = -1;
            for (int remembered : remembered_slots[identity]) {
                if (!occupied.count(remembered) && static_cast<unsigned>(remembered) < local_count) { slot = remembered; break; }
            }
            // A controller already navigating Host/Connect owns this context's
            // primary player. Otherwise keyboard/mouse keeps P1 and a press joins.
            const bool navigated = !was_lobby && lobby && !occupied.count(0) &&
                input->LastControllerButtonInstance() == instance &&
                input->LastControllerButtonTime() > input->last_keyboard_event_time &&
                input->LastControllerButtonTime() > input->last_mouse_event_time;
            if (slot < 0 && navigated) {
                slot = 0;
                if (input->LocalKeyboardPlayerIndex() == 0) input->BindLocalKeyboard(-1);
            }
            if (slot < 0 && !online->IsActive() && instance == world_primary_device) {
                const int assigned = input->LocalPlayerIndexForInputID(entry.second->player_input);
                if (assigned >= 0 && !occupied.count(assigned)) slot = assigned;
            }
            if (slot < 0 && (lobby || world) && rising) {
                slot = static_cast<int>(online->LocalLobbyPlayerCount());
                const auto saved = removed_device_profiles.find(identity);
                const std::string name = saved != removed_device_profiles.end() ? saved->second.name : "";
                if (online->AddLocalLobbyPlayer(name) == static_cast<PlayerID>(-1)) slot = -1;
                else RestoreDeviceProfile(identity, static_cast<unsigned>(slot));
            }
            if (slot >= 0) {
                joined_devices[instance] = {identity, slot};
                if (rising) joining_buttons_held.insert(instance);
                auto& memory = remembered_slots[identity];
                if (std::find(memory.begin(), memory.end(), slot) == memory.end()) memory.push_back(slot);
                occupied.insert(slot);
                NotifyDevice(static_cast<unsigned>(slot), true);
            } else if (lobby || world || (online->IsActive() && used_lobby_context)) input->BindLocalJoystick(instance, -1);
        }
        joined = joined_devices.find(instance);
        if (joined != joined_devices.end()) input->BindLocalJoystick(instance, joined->second.local);
        // The press that claims/reconnects a controller joins only. A pad
        // which was already auto-assigned its slot can otherwise also activate
        // the lobby's focused Rename/Close button. Consume it until release.
        if (joining_buttons_held.count(instance)) {
            entry.second->joystick.buttons_down_.clear();
            if (!down) joining_buttons_held.erase(instance);
        }
    }
    const bool text_input = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    if ((lobby || world) && keyboard_join && (!lobby || join_input_enabled) && !text_input && input->LocalKeyboardPlayerIndex() < 0) {
        const unsigned index = online->LocalLobbyPlayerCount();
        if (online->AddLocalLobbyPlayer("Keyboard player") != static_cast<PlayerID>(-1)) {
            input->BindLocalKeyboard(static_cast<int>(index));
            if (ImGui::GetCurrentContext()) {
                ImGui::GetIO().KeysDown[SDLK_RETURN] = false;
                ImGui::GetIO().KeysDown[SDLK_z + 1 + SDLK_KP_ENTER - SDLK_CAPSLOCK] = false;
            }
            SDL_Keysym key = {};
            key.sym = SDLK_RETURN; key.scancode = SDL_SCANCODE_RETURN;
            input->getKeyboard().handleKeyUp(key);
            key.sym = SDLK_KP_ENTER; key.scancode = SDL_SCANCODE_KP_ENTER;
            input->getKeyboard().handleKeyUp(key);
        }
    }
    was_lobby = lobby;
}

void AttachLobby(ASContext* context) {
    context->RegisterObjectType("LobbyPlayer", sizeof(LobbyPlayer), asOBJ_VALUE | asGetTypeTraits<LobbyPlayer>());
    context->RegisterObjectBehaviour("LobbyPlayer", asBEHAVE_CONSTRUCT, "void f()", asFUNCTION(ConstructPlayer), asCALL_CDECL_OBJLAST);
    context->RegisterObjectBehaviour("LobbyPlayer", asBEHAVE_CONSTRUCT, "void f(const LobbyPlayer &in)", asFUNCTION(CopyPlayer), asCALL_CDECL_OBJLAST);
    context->RegisterObjectBehaviour("LobbyPlayer", asBEHAVE_DESTRUCT, "void f()", asFUNCTION(DestroyPlayer), asCALL_CDECL_OBJLAST);
    context->RegisterObjectMethod("LobbyPlayer", "LobbyPlayer &opAssign(const LobbyPlayer &in)", asMETHODPR(LobbyPlayer, operator=, (const LobbyPlayer&), LobbyPlayer&), asCALL_THISCALL);
    context->RegisterObjectProperty("LobbyPlayer", "uint id", asOFFSET(LobbyPlayer, id));
    context->RegisterObjectProperty("LobbyPlayer", "string name", asOFFSET(LobbyPlayer, name));
    context->RegisterObjectProperty("LobbyPlayer", "int local_index", asOFFSET(LobbyPlayer, local_index));
    context->RegisterObjectProperty("LobbyPlayer", "bool local", asOFFSET(LobbyPlayer, local));
    context->RegisterObjectProperty("LobbyPlayer", "bool host", asOFFSET(LobbyPlayer, host));
    context->RegisterObjectProperty("LobbyPlayer", "bool leader", asOFFSET(LobbyPlayer, leader));
    context->RegisterObjectProperty("LobbyPlayer", "bool bot", asOFFSET(LobbyPlayer, bot));
    context->RegisterObjectProperty("LobbyPlayer", "int team", asOFFSET(LobbyPlayer, team));
    context->RegisterObjectProperty("LobbyPlayer", "int lobby_team", asOFFSET(LobbyPlayer, lobby_team));
    context->RegisterObjectProperty("LobbyPlayer", "int screen", asOFFSET(LobbyPlayer, screen));
    context->RegisterObjectProperty("LobbyPlayer", "bool connected", asOFFSET(LobbyPlayer, connected));
#define LOBBY_FN(declaration, function) context->RegisterGlobalFunction(declaration, asFUNCTION(function), asCALL_CDECL)
    LOBBY_FN("bool LobbyActive()", Active); LOBBY_FN("bool LobbyIsHost()", IsHost);
    LOBBY_FN("bool LobbyIsLocalOnly()", LocalOnly); LOBBY_FN("bool LobbyOpenToNetwork()", OpenToNetwork);
    LOBBY_FN("bool LobbyCanManageLocal()", CanManageLocal); LOBBY_FN("bool LobbyBeginHost()", BeginHost);
    LOBBY_FN("bool LobbyAddLocalPlayer()", AddLocal); LOBBY_FN("bool LobbyCanStart()", CanStart);
    LOBBY_FN("bool LobbyAddBot()", AddBot); LOBBY_FN("bool LobbyCanRenamePlayer(uint)", CanRename);
    LOBBY_FN("bool LobbyStart()", Start); LOBBY_FN("void LobbyLeave()", Leave);
    LOBBY_FN("bool LobbyCanLeave()", CanLeave); LOBBY_FN("bool LobbyTryLeave()", TryLeave);
    LOBBY_FN("int LobbyGetCurrentInputID()", CurrentInputID);
    LOBBY_FN("void LobbyUpdate()", UpdateLocalLobbyInput); LOBBY_FN("uint LobbySnapshotRevision()", Revision);
    LOBBY_FN("array<LobbyPlayer>@ LobbyPlayers()", PlayerArray);
    LOBBY_FN("string LobbyGetMode()", Mode); LOBBY_FN("string LobbyGetCampaign()", Campaign); LOBBY_FN("string LobbyGetMap()", Map);
    LOBBY_FN("bool LobbyGetBuildMode()", Build); LOBBY_FN("bool LobbyGetAllowAllPause()", AllowAllPause);
    LOBBY_FN("bool LobbyGetForceTeams()", ForceTeams); LOBBY_FN("bool LobbySetForceTeams(bool)", SetForceTeams);
    LOBBY_FN("bool LobbyGetHostControlsTeams()", HostControlsTeams); LOBBY_FN("bool LobbySetHostControlsTeams(bool)", SetHostControlsTeams);
    LOBBY_FN("bool LobbyCanSetPlayerTeam(uint)", CanSetTeam);
    LOBBY_FN("bool LobbyCanPromoteHost(uint)", CanPromoteHost); LOBBY_FN("bool LobbyPromoteHost(uint)", PromoteHost);
    LOBBY_FN("bool LobbyCanPromoteLeader(uint)", CanPromoteLeader); LOBBY_FN("bool LobbyPromoteLeader(uint)", PromoteLeader);
    LOBBY_FN("uint LobbyCurrentPlayerID()", MenuPlayerID); LOBBY_FN("int LobbyCurrentLocalPlayerIndex()", CurrentLocalPlayerIndex);
    LOBBY_FN("int LobbyGetLocalLeaderInputID()", LeaderInputID);
    LOBBY_FN("bool LobbyCurrentPlayerIsLeader()", CanManageLocal);
    LOBBY_FN("string LobbyGetMigrationStatus()", MigrationStatus); LOBBY_FN("bool LobbyMigrationBusy()", MigrationBusy);
    LOBBY_FN("int LobbyGetAssignedTeam(int)", AssignedTeam);
    LOBBY_FN("bool LobbyAssignModeTeams(const array<int> &in, const array<int> &in)", AssignTeams);
    LOBBY_FN("bool LobbySetSelection(const string &in, const string &in, const string &in, bool)", SetSelection);
    LOBBY_FN("bool LobbySetPlayerName(uint, const string &in)", SetName);
    LOBBY_FN("bool LobbySetPlayerTeam(uint, int)", SetTeam); LOBBY_FN("bool LobbySetPlayerScreen(uint, int)", SetScreen);
    LOBBY_FN("bool LobbySetAllowAllPause(bool)", SetAllowAllPause);
    LOBBY_FN("bool LobbyCanRemovePlayer(uint)", CanRemove); LOBBY_FN("bool LobbyRemovePlayer(uint)", Remove);
    LOBBY_FN("bool LobbyEndGame()", EndGame); LOBBY_FN("void LobbySetJoinInputEnabled(bool)", SetJoinInputEnabled);
    LOBBY_FN("vec3 LobbyTeamColor(int)", Online::LobbyTeamColor);
    LOBBY_FN("uint GetLocalPlayerCount()", LocalCount); LOBBY_FN("int GetLocalPlayerIndexForInputID(int)", LocalIndex);
    LOBBY_FN("bool LobbyCanPause()", CanPause); LOBBY_FN("bool LobbySetPaused(bool)", SetPaused);
#undef LOBBY_FN
}
