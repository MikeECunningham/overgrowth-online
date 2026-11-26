//-----------------------------------------------------------------------------
//           Name: test_bridge.cpp
//      Developer: Overgrowth Online (crxhvrd)
//    Description: See test_bridge.h. Protocol, one JSON object per line:
//                 request  {"id": 7, "cmd": "state", ...arguments}
//                 response {"id": 7, "ok": true, ...data}
//                       or {"id": 7, "ok": false, "error": "..."}
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#include "test_bridge.h"
#include <Editors/entity_type.h>
#include <Editors/actors_editor.h>

#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Game/level.h>
#include <Objects/movementobject.h>
#include <Objects/riggedobject.h>
#include <Graphics/flares.h>
#include <Objects/itemobject.h>
#include <Online/char_snapshot.h>
#include <Online/online.h>
#if ENABLE_STEAMWORKS
#include <Steam/steamworks.h>
#include <Steam/steamworks_matchmaking.h>
#endif
#include <Online/clock_sync.h>
#include <Editors/map_editor.h>
#include <Objects/reflectioncaptureobject.h>
#include <Scripting/scriptparams.h>
#include <Scripting/angelscript/ascontext.h>
#include <Internal/config.h>
#include <Internal/crashreport.h>
#include <UserInput/input.h>
#include <Graphics/graphics.h>
#include <Graphics/shaders.h>
#include <Graphics/camera.h>
#include <Graphics/frame_timers.h>
#include <GUI/difficulty_hud.h>
#include <GUI/progression_ui.h>
#include <Online/friendly_progression.h>
#include <Online/armor_protection.h>
#include <Online/campaign_equipment.h>
#include <Asset/Asset/attachmentasset.h>
#include <Testing/sampler.h>
#include <Threading/job_system.h>
#include <Threading/sdl_wrapper.h>
#include <Graphics/render_backend.h>
#include <Graphics/Vulkan/vk_backend.h>
#include <Graphics/Vulkan/vk_gl.h>
#include <Graphics/Vulkan/vk_nrd.h>
#include <Graphics/Vulkan/vk_rt.h>
#include <Graphics/raytrace.h>
#include <Graphics/Vulkan/vk_draw.h>
#include <Graphics/textures.h>
#include <Internal/timer.h>
#include <Logging/logdata.h>
#include <JSON/json.h>
#include <imgui_internal.h>

#include <SDL_net.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <stdexcept>

extern Timer game_timer;
extern Config config;

namespace {

bool test_pointer_enabled = false;
int test_pointer_x = 0;
int test_pointer_y = 0;
uint32_t test_pointer_buttons = 0;

#if SDL_VERSION_ATLEAST(2, 0, 14)
// SDL virtual devices live in this process only. They exercise the real input
// event/ownership path without sending input to the desktop or a user's game.
std::map<int, SDL_Joystick*> virtual_pads;

int VirtualPadDeviceIndex(SDL_JoystickID instance) {
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == instance) return i;
    return -1;
}

void DetachVirtualPad(int slot) {
    const auto found = virtual_pads.find(slot);
    if (found == virtual_pads.end()) return;
    const SDL_JoystickID instance = SDL_JoystickInstanceID(found->second);
    SDL_JoystickClose(found->second);
    virtual_pads.erase(found);
    const int device = VirtualPadDeviceIndex(instance);
    if (device >= 0) SDL_JoystickDetachVirtual(device);
}

void ClearVirtualPads() {
    if (!(SDL_WasInit(SDL_INIT_JOYSTICK) & SDL_INIT_JOYSTICK)) { virtual_pads.clear(); return; }
    while (!virtual_pads.empty()) DetachVirtualPad(virtual_pads.begin()->first);
}
#endif

double EpochMs() {
    using namespace std::chrono;
    return (double)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Json::Value Vec(const vec3& v) {
    Json::Value a(Json::arrayValue);
    a.append(v[0]);
    a.append(v[1]);
    a.append(v[2]);
    return a;
}

MovementObject* FindCharacter(SceneGraph* sg, int id) {
    if (sg == nullptr) {
        return nullptr;
    }

    for (Object* obj : sg->movement_objects_) {
        if (obj->GetID() == id) {
            return (MovementObject*)obj;
        }
    }

    return nullptr;
}

// This machine's own character: the session's own player online (other
// players' characters are controlled too there), else the first avatar
int MyAvatar(Engine* engine) {
    Online* online = Online::Instance();

    if (online->IsActive()) {
        return (int)online->GetOwnPlayerState().object_id;
    }

    return engine->num_avatars > 0 ? engine->avatar_ids[0] : -1;
}

}  // namespace

bool TestBridgePointerState(int& x, int& y, uint32_t& buttons) {
    if (!TestBridge::Instance()->Running() || !test_pointer_enabled) return false;
    x = test_pointer_x;
    y = test_pointer_y;
    buttons = test_pointer_buttons;
    return true;
}

TestBridge* TestBridge::Instance() {
    static TestBridge bridge;
    return &bridge;
}

bool TestBridge::Start(int port) {
    if (running_ || port <= 0 || port > 65535) {
        return false;
    }

    port_ = port;
    running_ = true;
    thread_ = std::thread(&TestBridge::ThreadMain, this);
    LOGI << "Test bridge listening on port " << port << std::endl;
    return true;
}

void TestBridge::Stop() {
    test_pointer_enabled = false;
    test_pointer_buttons = 0;
    if (!running_) {
        return;
    }

    running_ = false;

    if (thread_.joinable()) {
        thread_.join();
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    ClearVirtualPads();
#endif
}

void TestBridge::ThreadMain() {
    IPaddress addr;

    if (SDLNet_ResolveHost(&addr, NULL, (Uint16)port_) != 0) {
        LOGE << "Test bridge: cannot resolve the listen address" << std::endl;
        running_ = false;
        return;
    }

    TCPsocket listen = SDLNet_TCP_Open(&addr);

    if (listen == NULL) {
        LOGE << "Test bridge: cannot listen on port " << port_ << ": " << SDLNet_GetError() << std::endl;
        running_ = false;
        return;
    }

    SDLNet_SocketSet set = SDLNet_AllocSocketSet(2);
    SDLNet_TCP_AddSocket(set, listen);
    TCPsocket client = NULL;
    std::string buffer;

    while (running_) {
        // Blocks up to 10 ms: no busy spin
        int ready = SDLNet_CheckSockets(set, 10);

        if (ready > 0 && SDLNet_SocketReady(listen)) {
            TCPsocket s = SDLNet_TCP_Accept(listen);

            if (s != NULL) {
                // SDL_net always listens on every interface: refuse anything but loopback
                IPaddress* peer = SDLNet_TCP_GetPeerAddress(s);
                bool loopback = peer != NULL && (SDLNet_Read32(&peer->host) >> 24) == 127;

                if (!loopback || client != NULL) {
                    SDLNet_TCP_Close(s);
                } else {
                    client = s;
                    SDLNet_TCP_AddSocket(set, client);
                    buffer.clear();
                }
            }
        }

        if (client != NULL && ready > 0 && SDLNet_SocketReady(client)) {
            char chunk[4096];
            int n = SDLNet_TCP_Recv(client, chunk, sizeof(chunk));

            if (n <= 0) {
                SDLNet_TCP_DelSocket(set, client);
                SDLNet_TCP_Close(client);
                client = NULL;
            } else {
                buffer.append(chunk, n);
                size_t pos;

                while ((pos = buffer.find('\n')) != std::string::npos) {
                    std::lock_guard<std::mutex> guard(lock_);
                    incoming_.push_back(buffer.substr(0, pos));
                    buffer.erase(0, pos + 1);
                }
            }
        }

        std::deque<std::string> out;
        {
            std::lock_guard<std::mutex> guard(lock_);
            out.swap(outgoing_);
        }

        for (const std::string& line : out) {
            if (client == NULL) {
                break;
            }

            if (SDLNet_TCP_Send(client, line.data(), (int)line.size()) < (int)line.size()) {
                SDLNet_TCP_DelSocket(set, client);
                SDLNet_TCP_Close(client);
                client = NULL;
            }
        }
    }

    if (client != NULL) {
        SDLNet_TCP_Close(client);
    }

    SDLNet_TCP_Close(listen);
    SDLNet_FreeSocketSet(set);
}

void TestBridge::NoteLevelMessage(const std::string& msg) {
    if (!running_ || msg.compare(0, 8, "tutorial") == 0) {
        return;  // (the stock scripts send "tutorial ..." many times a second)
    }
    level_messages_.push_back(msg);
    while (level_messages_.size() > 400) {
        level_messages_.pop_front();
    }
}

void TestBridge::Pump(Engine* engine) {
    if (!running_) {
        return;
    }

    ++frame_;
    std::deque<std::string> in;
    {
        std::lock_guard<std::mutex> guard(lock_);
        in.swap(incoming_);
    }

    for (const std::string& line : in) {
        std::string response = Handle(line, engine);
        std::lock_guard<std::mutex> guard(lock_);
        outgoing_.push_back(response);
    }
}

std::string TestBridge::Handle(const std::string& line, Engine* engine) {
    Json::Value req;
    Json::Value out(Json::objectValue);
    Json::Reader reader;

    if (!reader.parse(line, req) || !req.isObject()) {
        out["ok"] = false;
        out["error"] = "bad json";
    } else {
        out["id"] = req.get("id", Json::Value());
        std::string cmd = req.get("cmd", "").asString();

        try {
            out["ok"] = true;
            Dispatch(cmd, req, out, engine);
        } catch (const std::exception& e) {
            out["ok"] = false;
            out["error"] = e.what();
        }
    }

    Json::FastWriter writer;
    return writer.write(out);  // ends with '\n'
}

void TestBridge::Dispatch(const std::string& cmd, const Json::Value& req, Json::Value& out, Engine* engine) {
    SceneGraph* sg = engine->GetSceneGraph();
    bool loaded = engine->IsLevelLoaded() && sg != nullptr;
    Online* online = Online::Instance();

    if (cmd == "ping") {
        out["frame"] = (Json::UInt64)frame_;
        out["epoch_ms"] = EpochMs();
    } else if (cmd == "status") {
        out["frame"] = (Json::UInt64)frame_;
        out["epoch_ms"] = EpochMs();
        out["game_time"] = game_timer.game_time;
        out["time_scale"] = game_timer.time_scale;
        out["clock_ms"] = OnlineClockMs();  // this instance's online clock (clock sync checks)
        out["menu_owner"] = config["controller_menu_owner"].toNumber<int>();
        out["current_menu_player"] = engine->current_menu_player;
        if (ImGui::GetCurrentContext()) {
            const ImGuiContext& gui = *ImGui::GetCurrentContext();
            out["ui_nav_id"] = gui.NavId;
            out["ui_nav_window_id"] = gui.NavWindow ? gui.NavWindow->ID : 0;
            out["ui_nav_window"] = gui.NavWindow ? gui.NavWindow->Name : "";
        }
        out["camera_pos"] = Vec(ActiveCameras::Get()->GetPos());  // the view on screen (camera jitter checks)
        out["level_loaded"] = loaded;
        out["waiting_for_input"] = engine->waiting_for_input_;
        out["grab_mouse"] = Input::Instance()->GetGrabMouse();  // (the editor's cursor checks)
        out["cursor_visible"] = engine->CursorVisible();
        out["paused"] = engine->paused;
        out["level"] = loaded ? sg->level_path_.GetOriginalPath() : "";
        out["my_avatar"] = loaded ? MyAvatar(engine) : -1;
        out["gl_problems"] = GLDebugProblemCount();  // GL debug output: errors, undefined, deprecated ("gl_debug")
        out["renderer"] = UseVulkan() ? "vulkan" : "gl";
        out["local_players"] = online->IsActive() ? static_cast<int>(online->LocalLobbyPlayerCount()) : engine->DebugLocalPlayerCount();
        out["local_inputs"] = Input::Instance()->GetNumLocalPlayers();
        out["open_menu_players"] = Json::Value(Json::arrayValue);
        for (unsigned i = 0; i < Input::Instance()->GetNumLocalPlayers(); ++i) {
            if (engine->PlayerHasMenu(Input::Instance()->GetLocalPlayerInputID(i))) out["open_menu_players"].append(i);
        }
        out["split_screen"] = engine->GetSplitScreen();
        out["dialogue_camera_control"] = loaded && sg->level != nullptr && sg->level->DialogueCameraControl();
        out["coop_cutscene"] = online->CoopCutscene();
        out["campaign_main_avatar"] = loaded ? online->CampaignMainAvatarID() : -1;
        out["render_views"] = engine->RenderedViewCount();
        Json::Reader difficulty_reader;
        difficulty_reader.parse(online->DifficultyHUDSnapshot(), out["difficulty_hud"], false);
        out["difficulty_hud_layout"] = DifficultyHUD::Inspect();
        out["progression_ui"] = ProgressionUI::Inspect();
        if (UseVulkan()) {
            out["vk_last_trace_gi"] = VkRt::GiTraced();
            out["vk_last_nrd_dispatches"] = VkRt::LastNrdDispatchCount();
            out["vk_nrd_image_bytes"] = (Json::UInt64)VkNrd::LiveImageBytes();
            const ivec2 lighting_size = Raytrace::LightingSize();
            out["vk_lighting_width"] = lighting_size[0];
            out["vk_lighting_height"] = lighting_size[1];
            out["vk_last_trace_instances"] = VkRt::SceneInstances();
            out["vk_problems"] = VkBackend::ValidationProblems();  // validation layer warnings and errors ("vk_validation")
            out["vk_frames"] = (Json::UInt64)VkBackend::FramesPresented();
            VkDraw::Stats ds = VkDraw::GetStats();
            out["vk_draws"] = (Json::UInt64)ds.draws;
            out["vk_draws_skipped"] = (Json::UInt64)ds.skipped;
            out["vk_pipelines"] = (Json::UInt64)ds.pipelines;
            out["vk_passes"] = (Json::UInt64)ds.passes;
        }
        Json::Value& o = out["online"];
        Json::Reader challenge_reader;
        challenge_reader.parse(online->ModeChallengePayload(), o["challenge"], false);
        o["mode_settings"] = online->LobbyModeSettings();
        o["active"] = online->IsActive();
        o["hosting"] = online->IsHosting();
        o["client"] = online->IsClient();
        o["lobby_waiting"] = online->LobbyWaiting();
        o["local_only"] = online->IsLocalOnlySession();
        o["lobby_mode"] = online->LobbyMode();
        o["lobby_campaign"] = online->LobbyCampaign();
        o["lobby_map"] = online->LobbyMap();
        o["lobby_build"] = online->LobbyBuild();
        o["allow_all_pause"] = online->LobbyAllowAllPause();
        o["decision_host"] = online->LobbyHostPlayerID();
        o["host_controls_teams"] = online->LobbyHostControlsTeams();
        o["migration_busy"] = online->LobbyMigrationBusy();
        o["migration_status"] = online->LobbyMigrationStatus();
        o["bytes_out"] = (Json::UInt64)online->net_bytes_out.load();
        o["bytes_in"] = (Json::UInt64)online->net_bytes_in.load();
        o["msgs_out"] = (Json::UInt64)online->net_msgs_out.load();
        o["msgs_in"] = (Json::UInt64)online->net_msgs_in.load();
        o["dropped"] = (Json::UInt64)online->net_sim_dropped.load();
        o["records_out"] = (Json::UInt64)online->snapshot_records_out.load();
        o["records_in"] = (Json::UInt64)online->snapshot_records_in.load();
        o["snapshot_bytes_out"] = (Json::UInt64)online->snapshot_bytes_out.load();
        o["idle_skips"] = (Json::UInt64)online->snapshot_idle_skips.load();
#if ENABLE_STEAMWORKS
        {
            SteamworksMatchmaking* matchmaking = Steamworks::Instance()->GetMatchmaking();
            o["steam_lobby"] = matchmaking != nullptr ? std::to_string(matchmaking->CurrentLobby()) : "0";
            o["steam"] = Steamworks::Instance()->IsConnected();
        }
#endif
        if (online->online_session != nullptr) {  // (stress tests: the queues must not grow)
            std::lock_guard<std::mutex> guard(online->online_session->outgoing_messages_lock);
            o["out_queue"] = (int)online->online_session->outgoing_messages.size();
            o["persistent_messages"] = (int)online->online_session->persistent_outgoing_messages.size();
        }
        o["bones_captured"] = (Json::UInt64)online->snapshot_bones_captured.load();
        o["hits_requested"] = (Json::UInt64)online->hits_requested.load();
        o["hits_resolved"] = (Json::UInt64)online->hits_resolved.load();
        o["hits_out_of_range"] = (Json::UInt64)online->hits_out_of_range.load();
        o["hit_results"] = (Json::UInt64)online->hit_results.load();
        o["stale_hit_results"] = (Json::UInt64)online->stale_hit_results.load();
        o["item_updates_sent"] = (Json::UInt64)online->item_updates_sent.load();
        o["item_updates_idle_skipped"] = (Json::UInt64)online->item_updates_idle_skipped.load();
        o["takeovers"] = (Json::UInt64)online->takeovers.load();
        o["item_launches"] = (Json::UInt64)online->item_launches.load();
        o["item_hits"] = (Json::UInt64)online->item_hits.load();
        o["sounds_from_clients"] = (Json::UInt64)online->sounds_from_clients.load();
        o["overrides"] = (int)online->owner_overrides.size();
        o["positions_sent"] = (Json::UInt64)online->snapshot_positions_sent.load();
        o["sim_latency_ms"] = online->net_sim_latency_ms.load();
        o["sim_jitter_ms"] = online->net_sim_jitter_ms.load();
        o["sim_loss"] = online->net_sim_loss.load();
        o["peers"] = Json::Value(Json::arrayValue);
        o["players"] = Json::Value(Json::arrayValue);

        if (online->IsActive()) {
            for (const Peer& peer : online->GetPeers()) {
                Json::Value p;
                p["peer_id"] = (Json::UInt64)peer.peer_id;
                p["conn_id"] = (int)peer.conn_id;
                p["ping_ms"] = peer.current_ping_delta * 1000.0f;
                p["transport_ping_ms"] = online->GetPeerTransportPing(peer.conn_id);
                ConnectionStatus link{};
                online->GetConnectionStatus(peer, &link);
                p["queue_ms"] = (double)link.usec_queue_time / 1000.0;
                p["pending_reliable"] = link.pending_reliable;
                p["pending_unreliable"] = link.pending_unreliable;
                p["unacked_reliable"] = link.sent_unacked_reliable;
                p["send_rate_bytes_per_second"] = link.send_rate_bytes_per_second;
                p["out_bytes_per_second"] = link.out_bytes_per_sec;
                p["in_bytes_per_second"] = link.in_bytes_per_sec;
                p["out_packets_per_second"] = link.out_packets_per_sec;
                p["in_packets_per_second"] = link.in_packets_per_sec;
                p["clock_offset_ms"] = peer.clock.Offset();
                p["clock_samples"] = peer.clock.Samples();
                o["peers"].append(p);
            }

            for (const PlayerState& player : online->GetPlayerStates()) {
                Json::Value p;
                p["id"] = player.player_id;
                p["peer_id"] = player.peer_id;
                p["local_index"] = player.local_index;
                p["team"] = player.team;
                p["screen"] = player.screen;
                p["name"] = player.playername;
                p["bot"] = player.is_bot;
                p["host"] = online->IsLobbyHost(player.player_id);
                p["leader"] = online->IsLobbyLeader(player.player_id);
                p["avatar_id"] = player.object_id;
                o["players"].append(p);
            }
        }
    } else if (cmd == "session_slots") {
        const PlayerID actor = req.get("actor", LobbyAuthority::InvalidPlayer).asUInt();
        const std::string action = req.get("action", "").asString();
        if (action == "add") {
            const PlayerID created = online->AddLobbyBot(req.get("name", "").asString(), actor);
            out["created"] = created; out["submitted"] = created != LobbyAuthority::InvalidPlayer;
        } else if (action == "admission") out["submitted"] = online->SetLobbyAutoAdmission(req.get("automatic", true).asBool(), actor);
        else if (action == "kind") out["submitted"] = online->SetLobbySlotPlayer(req.get("slot", 0).asUInt(), req.get("player", false).asBool(), actor);
        else if (action == "swap") out["submitted"] = online->SwapLobbySlots(req.get("first", 0).asUInt(), req.get("second", 0).asUInt(), actor);
        else if (action == "validate") out["valid"] = online->ValidateLobbySlots(req["saved"]);
        else if (action == "restore") out["submitted"] = online->RestoreLobbySlots(req["saved"]);
        out["saved"] = online->SaveLobbySlots();
    } else if (cmd == "progression") {
        const PlayerID actor = req.get("actor", LobbyAuthority::InvalidPlayer).asUInt();
        const PlayerID target = req.get("target", actor).asUInt();
        if (req["action"].isString())
            out["submitted"] = FriendlyProgression::Act(actor, target, req["action"].asString(), req.get("key", "").asString(),
                                                        req.get("generation", 0).asUInt());
        out["view"] = FriendlyProgression::View(actor, target);
        out["ledger"] = FriendlyProgression::Snapshot();
    } else if (cmd == "state") {
        out["characters"] = Json::Value(Json::arrayValue);
        out["my_avatar"] = loaded ? MyAvatar(engine) : -1;

        if (loaded) {
            for (Object* obj : sg->movement_objects_) {
                MovementObject* mo = (MovementObject*)obj;
                Json::Value c;
                c["id"] = mo->GetID();
                c["common_id"] = online->GetOriginalID(mo->GetID());  // the host's id: the same on every machine
                c["controlled"] = mo->controlled;
                c["remote"] = mo->remote;
                c["is_player"] = mo->is_player;
                c["teams"] = mo->GetScriptParams()->HasParam("Teams") ? mo->GetScriptParams()->GetStringVal("Teams") : "";
                c["controller_id"] = mo->controller_id;
                c["camera_id"] = mo->camera_id;
                c["pos"] = Vec(mo->position);
                c["visible"] = mo->visible;
                c["enabled"] = mo->enabled_;
                c["vel"] = Vec(mo->velocity);
                c["proxy"] = mo->network_proxy;
                c["bones"] = (int)mo->rigged_object()->skeleton_.physics_bones.size();
                c["morphs"] = (int)mo->rigged_object()->morph_targets.size();
                c["weapon_slots"] = Json::Value(Json::arrayValue);
                c["weapon_slots_common"] = Json::Value(Json::arrayValue);
                for (int slot = 0; mo->ASHasVar("weapon_slots") && slot < 6; ++slot) {
                    const int item = mo->ASGetArrayIntVar("weapon_slots", slot);
                    c["weapon_slots"].append(item);
                    c["weapon_slots_common"].append(item < 0 ? -1 : static_cast<int>(online->GetOriginalID(item)));
                }
                c["attachments"] = Json::Value(Json::arrayValue);
                for (const AttachedItem& attached : mo->rigged_object()->attached_items.items) {
                    ItemObject* item = attached.item.GetAttached();
                    if (!item) continue;
                    Json::Value entry;
                    entry["id"] = item->GetID();
                    entry["common_id"] = static_cast<int>(online->GetOriginalID(item->GetID()));
                    entry["state"] = static_cast<int>(item->state());
                    entry["bone_id"] = attached.bone_id;
                    c["attachments"].append(entry);
                }
                c["armor_attachments"] = Json::Value(Json::arrayValue);
                const RiggedObject* rig = mo->rigged_object();
                for (const AttachedEnvObject& attached : rig->children) {
                    Object* child = attached.direct_ptr;
                    if (!child || child->GetType() != _env_object) continue;
                    Json::Value entry;
                    entry["id"] = child->GetID();
                    entry["common_id"] = static_cast<int>(online->GetOriginalID(child->GetID()));
                    entry["path"] = child->obj_file;
                    entry["enabled"] = child->enabled_;
                    entry["region"] = child->GetScriptParams()->HasParam("Campaign Armor Region") ?
                        child->GetScriptParams()->ASGetInt("Campaign Armor Region") :
                        child->GetScriptParams()->HasParam("Progression Armor Region") ?
                        child->GetScriptParams()->ASGetInt("Progression Armor Region") : 0;
                    entry["material"] = child->GetScriptParams()->HasParam("Armor Material Tier") ?
                        child->GetScriptParams()->ASGetInt("Armor Material Tier") : 0;
                    entry["pos"] = Vec(child->GetTranslation());
                    entry["scale"] = Vec(child->GetScale());
                    entry["parent"] = child->parent ? static_cast<int>(online->GetOriginalID(child->parent->GetID())) : -1;
                    entry["bones"] = Json::Value(Json::arrayValue);
                    int total = 0;
                    bool valid = true;
                    for (const BoneConnect& bone : attached.bone_connects) {
                        if (!bone.num_connections) continue;
                        Json::Value connection;
                        connection["id"] = bone.bone_id;
                        connection["count"] = bone.num_connections;
                        connection["rel"] = Json::Value(Json::arrayValue);
                        for (int i = 0; i < 16; ++i) {
                            connection["rel"].append(std::isfinite(bone.rel_mat[i]) ? Json::Value(bone.rel_mat[i]) : Json::Value());
                            valid = valid && std::isfinite(bone.rel_mat[i]);
                        }
                        entry["bones"].append(connection);
                        valid = valid && bone.num_connections > 0 && bone.bone_id >= 0 &&
                            static_cast<size_t>(bone.bone_id) < rig->display_bone_transforms.size();
                        total += bone.num_connections;
                    }
                    valid = valid && total > 0;
                    mat4 expected(0.0f);
                    if (valid) {
                        for (const BoneConnect& bone : attached.bone_connects) {
                            if (bone.num_connections) expected +=
                                (rig->display_bone_transforms[bone.bone_id].GetMat4() * bone.rel_mat) *
                                (static_cast<float>(bone.num_connections) / total);
                        }
                        for (int i = 0; i < 16; ++i) valid = valid && std::isfinite(expected[i]);
                    }
                    const float fit = valid ? distance(child->GetTranslation(), expected.GetTranslationPart()) : 0.0f;
                    valid = valid && std::isfinite(fit);
                    entry["fit_valid"] = valid;
                    entry["fit_error"] = valid ? Json::Value(fit) : Json::Value();
                    if (valid && child->obj_file.find("large_wood_plate.xml") != std::string::npos) {
                        const auto& bone = attached.bone_connects[0];
                        const mat4 physical = rig->display_bone_transforms[bone.bone_id].GetMat4() *
                                              rig->skeleton_.physics_bones[bone.bone_id].initial_rotation;
                        const vec3 limb_axis = normalize(physical.GetRotatedvec3(vec3(0,0,1)));
                        const vec3 plate_axis = child->GetRotation() * vec3(0,1,0);
                        entry["long_axis_alignment"] = std::fabs(dot(limb_axis, plate_axis));
                    }
                    if (valid) entry["expected_pos"] = Vec(expected.GetTranslationPart());
                    c["armor_attachments"].append(entry);
                }
                c["authored_armor_regions"] = online->AuthoredArmorRegions(mo->GetID());
                c["item_connections"] = Json::Value(Json::arrayValue);
                for (const ItemObjectScriptReader& connection : mo->item_connections) {
                    if (!connection.GetAttached()) continue;
                    Json::Value entry;
                    entry["id"] = connection.GetAttached()->GetID();
                    entry["common_id"] = static_cast<int>(online->GetOriginalID(connection.GetAttached()->GetID()));
                    entry["type"] = static_cast<int>(connection.attachment_type);
                    entry["mirrored"] = connection.attachment_mirror;
                    c["item_connections"].append(entry);
                }
                c["char_path"] = mo->character_path;
                c["campaign_scaling_source"] = mo->GetScriptParams()->HasParam("Campaign Scaling Source") ?
                    mo->GetScriptParams()->ASGetInt("Campaign Scaling Source") : -1;
                for (const auto& field : {std::make_pair("campaign_scaling_slot", "Campaign Scaling Slot"),
                                         std::make_pair("campaign_scaling_epoch", "Campaign Scaling Epoch"),
                                         std::make_pair("campaign_scaling_goal", "Campaign Scaling Goal")}) {
                    c[field.first] = mo->GetScriptParams()->HasParam(field.second) ? mo->GetScriptParams()->ASGetInt(field.second) : -1;
                }
                c["campaign_boss_override"] = mo->GetScriptParams()->HasParam("Campaign Boss") ?
                    mo->GetScriptParams()->ASGetInt("Campaign Boss") : -1;
                c["palette"] = Json::Value(Json::arrayValue);
                for (const LabeledColor& lc : *mo->GetPalette()) {
                    Json::Value e(Json::arrayValue);
                    e.append(lc.label);
                    e.append(lc.color[0]);
                    e.append(lc.color[1]);
                    e.append(lc.color[2]);
                    c["palette"].append(e);
                }

                if (mo->network_proxy) {
                    c["net_delay_ms"] = mo->net_buffer.DelayMs();
                    c["net_interval_ms"] = mo->net_buffer.IntervalMs();
                    c["net_frames"] = mo->net_buffer.Size();
                }

                if (mo->ASHasVar("knocked_out")) {
                    c["knocked_out"] = mo->ASGetIntVar("knocked_out");
                }

                if (mo->ASHasVar("state")) {
                    c["state"] = mo->ASGetIntVar("state");
                }
                for (const char* name : {"goal", "escort_id", "target_id", "attacked_by_id"}) {
                    if (mo->ASHasVar(name)) c["ai_" + std::string(name)] = mo->ASGetIntVar(name);
                }
                for (const char* name : {"updated", "shadow_id", "lf_shadow_id", "rf_shadow_id", "ofin_victim"}) {
                    if (mo->ASHasVar(name)) c[name] = mo->ASGetIntVar(name);
                }
                c["visible"] = mo->visible;
                for (const char* name : {"duck_amount", "target_duck_amount", "ofin_weight"}) {
                    if (mo->ASHasVar(name)) c[name] = mo->ASGetFloatVar(name);
                }
                if (mo->ASHasVar("lobby_escort_bot")) c["lobby_escort_bot"] = mo->ASGetBoolVar("lobby_escort_bot");
                if (mo->ASHasVar("lobby_escort_player_id")) c["lobby_escort_player_id"] = mo->ASGetIntVar("lobby_escort_player_id");

                if (mo->ASHasVar("on_ground")) {
                    c["on_ground"] = mo->ASGetBoolVar("on_ground");
                }
                if (mo->ASHasVar("dialogue_control")) {
                    c["dialogue_control"] = mo->ASGetBoolVar("dialogue_control");
                }
                if (mo->ASHasVar("coop_cutscene_hold")) {
                    c["coop_cutscene_hold"] = mo->ASGetBoolVar("coop_cutscene_hold");
                }

                if (mo->ASHasVar("temp_health")) {
                    c["temp_health"] = mo->ASGetFloatVar("temp_health");
                }

                if (mo->ASHasVar("blood_health")) {
                    c["blood_health"] = mo->ASGetFloatVar("blood_health");
                }
                if (mo->ASHasVar("permanent_health")) {
                    c["permanent_health"] = mo->ASGetFloatVar("permanent_health");
                }

                c["combat_stats"] = Json::Value(Json::objectValue);
                for (const char* name : {"p_damage_multiplier", "p_attack_damage_mult", "p_attack_knockback_mult",
                                         "p_attack_speed_mult", "p_speed_mult", "p_aggression", "p_ground_aggression",
                                         "p_block_skill", "p_block_followup", "g_weapon_catch_skill", "g_throw_counter_probability", "game_difficulty",
                                         "campaign_vitality_mult", "campaign_damage_mult", "campaign_perception_mult", "campaign_armor_mult", "campaign_ai_bonus", "campaign_base_difficulty",
                                         "campaign_authored_resistance", "campaign_authored_damage", "campaign_authored_aggression",
                                         "campaign_authored_ground_aggression", "campaign_authored_block_skill", "campaign_authored_block_followup",
                                         "campaign_authored_attack_speed", "campaign_authored_throw_counter", "campaign_authored_weapon_catch"}) {
                    if (mo->ASHasVar(name)) c["combat_stats"][name] = mo->ASGetFloatVar(name);
                }
                for (const char* name : {"species", "ko_shield", "max_ko_shield", "campaign_ai_tweak", "campaign_armor_regions",
                                         "progression_health", "progression_damage", "progression_speed", "progression_stealth_passive", "progression_stealth_active"}) {
                    if (mo->ASHasVar(name)) c["combat_stats"][name] = mo->ASGetIntVar(name);
                }
                if (mo->ASHasVar("g_wearing_metal_armor"))
                    c["combat_stats"]["metal_armor"] = mo->ASGetBoolVar("g_wearing_metal_armor");

                out["characters"].append(c);
            }

            out["items"] = Json::Value(Json::arrayValue);
            for (Object* obj : sg->objects_) {
                if (obj->GetType() != _item_object) {
                    continue;
                }
                ItemObject* io = static_cast<ItemObject*>(obj);
                Json::Value it;
                it["id"] = io->GetID();
                it["enabled"] = io->enabled_;
                it["state"] = static_cast<int>(io->state());
                it["parent_id"] = io->parent ? io->parent->GetID() : -1;
                it["common_id"] = online->GetOriginalID(io->GetID());
                it["label"] = io->item_ref()->GetLabel();
                it["held_by"] = io->HeldByWhom();
                it["hidden"] = io->HolderHidden();  // not drawn: on a hidden character
                it["file"] = io->obj_file;
                it["stuck_in"] = io->StuckInWhom();
                it["pos"] = Vec(io->GetPhysicsTransform().GetTranslationPart());
                it["draw_pos"] = Vec(io->GetTransform().GetTranslationPart());  // where it is drawn
                for (const auto& field : {std::make_pair("campaign_weapon_owner", "Campaign Weapon Owner"),
                                         std::make_pair("campaign_weapon_epoch", "Campaign Weapon Epoch")}) {
                    it[field.first] = io->GetScriptParams()->HasParam(field.second) ? io->GetScriptParams()->ASGetInt(field.second) : -1;
                }
                out["items"].append(it);
            }
        }
    } else if (cmd == "virtual_pad") {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        const std::string action = req.get("action", "status").asString();
        int slot = req.get("slot", -1).asInt();
        if (action == "attach") {
            if (slot < 0) { slot = 0; while (virtual_pads.count(slot)) ++slot; }
            if (slot > 255 || virtual_pads.count(slot)) throw std::runtime_error("invalid or occupied virtual pad slot");
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);
            if (device < 0) throw std::runtime_error(SDL_GetError());
            char guid[64];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(device), guid, sizeof(guid));
            const std::string mapping = std::string(guid) + ",OGMP virtual controller,"
                "a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
                "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
            SDL_GameControllerAddMapping(mapping.c_str());
            SDL_Joystick* joystick = SDL_JoystickOpen(device);
            if (joystick == nullptr) { SDL_JoystickDetachVirtual(device); throw std::runtime_error(SDL_GetError()); }
            virtual_pads[slot] = joystick;
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
            out["slot"] = slot;
            out["device_index"] = device;
            out["instance_id"] = SDL_JoystickInstanceID(joystick);
        } else if (action == "detach") {
            DetachVirtualPad(slot);
        } else if (action == "clear") {
            ClearVirtualPads();
        } else if (action == "button" || action == "axis") {
            const auto found = virtual_pads.find(slot);
            if (found == virtual_pads.end()) throw std::runtime_error("no such virtual pad");
            const int index = req.get(action, -1).asInt();
            const int count = action == "button" ? SDL_JoystickNumButtons(found->second) : SDL_JoystickNumAxes(found->second);
            if (index < 0 || index >= count) throw std::runtime_error("virtual pad input index out of range");
            const int result = action == "button" ?
                SDL_JoystickSetVirtualButton(found->second, index, req.get("down", false).asBool() ? 1 : 0) :
                SDL_JoystickSetVirtualAxis(found->second, index, static_cast<Sint16>(std::max(-32768, std::min(32767, req.get("value", 0).asInt()))));
            if (result < 0) throw std::runtime_error(SDL_GetError());
            SDL_JoystickUpdate();
        } else if (action != "status") throw std::runtime_error("unknown virtual pad action");
        out["pads"] = Json::Value(Json::arrayValue);
        for (const auto& pad : virtual_pads) {
            const SDL_JoystickID instance = SDL_JoystickInstanceID(pad.second);
            Json::Value row;
            row["slot"] = pad.first; row["instance_id"] = instance;
            row["device_index"] = VirtualPadDeviceIndex(instance);
            const auto input = Input::Instance()->GetOpenJoysticks().find(instance);
            if (input != Input::Instance()->GetOpenJoysticks().end()) {
                row["input_id"] = input->second->player_input;
                row["local_index"] = Input::Instance()->LocalPlayerIndexForInputID(input->second->player_input);
                row["sensitivity"] = input->second->joystick.look_sensitivity_;
            }
            out["pads"].append(row);
        }
#else
        throw std::runtime_error("virtual pads require SDL 2.0.14 or newer");
#endif
    } else if (cmd == "exec") {
        if (req["target"].isString() && req["target"].asString() == "player_menu") {
            if (!engine->ExecutePlayerMenuCode(req.get("local_player", 0).asInt(), req.get("code", "").asString())) {
                throw std::runtime_error("no such open local player menu");
            }
            return;
        }
        if (req["target"].isString() && req["target"].asString() == "menu") {
            if (!engine->ExecuteMenuCode(req.get("code", "").asString())) {
                throw std::runtime_error("no scriptable menu");
            }
            return;
        }
        if (!loaded) {
            throw std::runtime_error("no level loaded");
        }

        std::string code = req.get("code", "").asString();
        const Json::Value& target = req["target"];

        if (target.isString() && target.asString() == "level") {
            const std::string name = req.get("context", "").asString();
            if (name.empty()) sg->level->Execute(code);
            else {
                bool found = false;
                for (auto& hook : sg->level->as_contexts_) if (hook.context_name == name || hook.path.GetOriginalPath() == name) {
                    hook.ctx->Execute(code);
                    found = true;
                    break;
                }
                if (!found) throw std::runtime_error("no such level script context");
            }
        } else if (target.isInt()) {
            MovementObject* mo = FindCharacter(sg, target.asInt());

            if (mo == nullptr) {
                throw std::runtime_error("no such character");
            }

            mo->Execute(code);
        } else {
            throw std::runtime_error("target must be \"level\" or a character id");
        }
    } else if (cmd == "input") {
        // Keys for one local player (default controller 0), applied every 120 Hz step
        // after the real devices (Input::ApplyInjectedInput)
        Input* input = Input::Instance();
        const int controller = req.get("controller", 0).asInt();
        PlayerInput* target_input = input->GetController(controller);
        if (controller < 0 || input->LocalPlayerIndexForInputID(controller) < 0 ||
            target_input == nullptr || !target_input->enabled || target_input->remote_controlled) {
            throw std::runtime_error("no such enabled local controller");
        }
        auto& injected_keys = input->InjectedKeysForController(controller);

        if (req.get("release_all", false).asBool()) {
            injected_keys.clear();
        }

        const Json::Value& release = req["release"];

        if (release.isArray()) {
            for (const Json::Value& name : release) {
                injected_keys.erase(name.asString());
            }
        }

        const Json::Value& hold = req["hold"];
        int steps = req.get("steps", -1).asInt();  // -1: until released

        if (hold.isObject()) {
            for (const std::string& name : hold.getMemberNames()) {
                Input::InjectedKey key;
                key.depth = hold[name].asFloat();
                key.steps_left = steps;
                injected_keys[name] = key;
            }
        }

        const Json::Value& look = req["look"];

        if (look.isArray() && look.size() == 2) {
            float* injected_look = input->InjectedLookForController(controller);
            injected_look[0] += look[0].asFloat();
            injected_look[1] += look[1].asFloat();
        }

        out["controller"] = controller;
        out["held"] = (int)injected_keys.size();
    } else if (cmd == "load_level") {
        if (req.get("discard", false).asBool()) {
            // (the "save changes?" check answered No: the level is left as it is)
            engine->check_save_level_changes_dialog_is_finished = true;
            engine->check_save_level_changes_last_result = true;
        }
        engine->ScriptableUICallback(req.get("path", "").asString());
    } else if (cmd == "host") {
        // Host first, then load: hosting inside a loaded level hands the
        // host's own character to the first client (Phase 0 finding)
        if (req.isMember("port")) {
            config.GetRef("mp_port") = req["port"].asInt();
        }

        online->StartHostingMultiplayer();
        std::string level = req.get("level", "").asString();

        if (req.isMember("campaign")) {
            engine->ScriptableUICallback("set_campaign " + req["campaign"].asString());  // its script advances levels
        }
        if (!level.empty()) {
            engine->ScriptableUICallback(level);
        }
    } else if (cmd == "connect") {
        if (req.isMember("port")) {
            config.GetRef("mp_port") = req["port"].asInt();
        }

        online->ConnectByIPAddress(req.get("ip", "127.0.0.1").asString());
        out["active"] = online->IsActive();
    } else if (cmd == "disconnect") {
        online->QueueStopMultiplayer();
    } else if (cmd == "start_session") {
        // What the host's "press any key" after loading does
        bool ready = !online->IsActive() || !online->IsHosting() || online->AllClientsReady();

        if (ready) {
            online->SessionStarted(true);
            engine->waiting_for_input_ = false;
        }

        out["started"] = ready;
    } else if (cmd == "profile") {
        // Overgrowth Online: the sampling profiler (Testing/sampler.h). {"seconds": s,
        // "interval_ms": ms} starts it; {"top": n} (or nothing) asks: "running" until done,
        // then the hottest native and script functions
        if (req.isMember("seconds")) {
            out["started"] = Sampler::Start(req["seconds"].asDouble(), req.get("interval_ms", 1.0).asDouble());
        } else {
            Json::Value result = Sampler::Result(req.get("top", 40).asInt(), req.get("focus", "").asString());
            for (const std::string& key : result.getMemberNames()) {
                out[key] = result[key];
            }
        }
    } else if (cmd == "perf") {
        // Overgrowth Online: the frame timers (Graphics/frame_timers.h): {"enable": bool},
        // and the last one-second window, every zone's GPU and CPU time per frame
        if (req.isMember("enable")) {
            FrameTimers::SetEnabled(req["enable"].asBool());
        }
        if (req.isMember("cpu_zones")) {
            FrameTimers::SetCpuZones(req["cpu_zones"].asBool());
        }
        FrameTimers::Snapshot snap = FrameTimers::Last();
        out["on"] = FrameTimers::g_on;
        out["window_s"] = snap.window_s;
        out["frames"] = snap.frames;
        out["frame_ms"] = snap.frame_ms;
        out["frame_ms_max"] = snap.frame_ms_max;
        out["frame_ms_p99"] = snap.frame_ms_p99;
        out["worker_busy_ms"] = snap.worker_busy_ms;  // (the job system: its workers' time per frame)
        out["workers"] = Jobs::WorkerCount();
        out["job_violations"] = Jobs::Violations();
        if (req.get("frame_list", false).asBool()) {
            Json::Value list(Json::arrayValue);
            for (float ms : snap.frame_list) {
                list.append(ms);
            }
            out["frame_list"] = list;
        }
        Json::Value zones(Json::arrayValue);
        for (const FrameTimers::Zone& z : snap.zones) {
            Json::Value v;
            v["path"] = z.path;
            v["depth"] = z.depth;
            v["gpu_ms"] = z.gpu_ms;
            v["cpu_ms"] = z.cpu_ms;
            v["calls"] = z.calls;
            v["cpu_only"] = z.cpu_only;
            zones.append(v);
        }
        out["zones"] = zones;
    } else if (cmd == "view") {
        // Overgrowth Online: a fixed view for screenshot comparisons: {"pos": [x, y, z],
        // "target": [x, y, z], "fov": degrees} holds the camera there with the game
        // paused; {"clear": true} lets go
        if (req.get("clear", false).asBool()) {
            engine->view_override_ = false;
            engine->user_paused = false;
            game_timer.render_time_frozen = false;
        } else {
            const Json::Value& p = req["pos"];
            const Json::Value& t = req["target"];
            if (!p.isArray() || p.size() != 3 || !t.isArray() || t.size() != 3) {
                throw std::runtime_error("view: pos and target are [x, y, z]");
            }
            engine->view_override_pos_ = vec3(p[0].asFloat(), p[1].asFloat(), p[2].asFloat());
            engine->view_override_target_ = vec3(t[0].asFloat(), t[1].asFloat(), t[2].asFloat());
            engine->view_override_fov_ = req.get("fov", 0.0).asFloat();
            engine->view_override_gui_ = req.get("gui", false).asBool();
            engine->view_override_ = true;
            engine->user_paused = req.get("pause", true).asBool();
            // Shaders' time held at a value of the test's (grass, sky, water the same every run)
            game_timer.frozen_render_time = req.get("time", 100.0).asFloat();
            game_timer.render_time_frozen = true;
        }
        out["view"] = engine->view_override_;
    } else if (cmd == "shader_check") {
        // Overgrowth Online (GL 4.6 stage 3.1): compile and link these programs
        // ({"programs": [[optional_shaders, name], ...]}), every failure back
        Shaders* shaders = Shaders::Instance();
        shaders->compile_failures.clear();
        const Json::Value& list = req["programs"];
        auto start = std::chrono::steady_clock::now();
        Json::Value stages(Json::arrayValue);  // (each program's dumped stage files, with shader_debug)
        static const char* kStageNames[] = {"vert", "frag", "geom", "tesc", "tese"};
        for (const Json::Value& p : list) {
            int id = shaders->returnProgram(p[1].asString(), (Shaders::OptionalShaders)p[0].asInt());
            shaders->createProgram(id);
            Json::Value files(Json::objectValue);
            for (int t = 0; t < kMaxShaderTypes && t < 5; ++t) {
                std::string dump = shaders->StageDumpName(id, t);
                if (!dump.empty()) {
                    files[kStageNames[t]] = dump;
                }
            }
            stages.append(files);
        }
        out["stages"] = stages;
        out["checked"] = (int)list.size();
        out["ms"] = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        Json::Value fails(Json::arrayValue);
        for (const std::string& f : shaders->compile_failures) {
            fails.append(f);
        }
        out["failures"] = fails;
    } else if (cmd == "gl_problems") {
        // Overgrowth Online: the GL debug output's problems, each message with its count
        if (req.get("provoke", false).asBool() && !UseVulkan()) {
            glEnable(0x7FFF);  // an invalid enum on purpose: the output must report it (gl_errors)
        }
        out["alive"] = GLDebugOutputAlive();  // the output reached the callback (our check marker)
        out["count"] = GLDebugProblemCount();
        Json::Value list(Json::arrayValue);
        for (const auto& problem : GLDebugProblems()) {
            Json::Value v;
            v["message"] = problem.first;
            v["count"] = problem.second;
            list.append(v);
        }
        out["problems"] = list;
    } else if (cmd == "texture_stats") {
        // Overgrowth Online (stage 5.5 checks): a texture by its name, level 0's first channel
        int w = 0, h = 0;
        float mn = 0, mx = 0, mean = 0, nonzero = 0;
        out["found"] = Textures::Instance()->LevelStats(req.get("name", "").asString(), &w, &h, &mn, &mx, &mean, &nonzero);
        out["width"] = w;
        out["height"] = h;
        out["min"] = mn;
        out["max"] = mx;
        out["mean"] = mean;
        out["nonzero"] = nonzero;
    } else if (cmd == "editor_item") {
        // A map editor ribbon item, as clicking it ("calculateao" bakes the light probes)
        if (!loaded) {
            throw std::runtime_error("editor_item needs a level");
        }
        sg->map_editor->RibbonItemClicked(req.get("item", "").asString(), req.get("param", false).asBool());
        out["probe_queue"] = (int)sg->light_probe_collection.to_process.size();
    } else if (cmd == "light_probes") {
        // The light probes' baked colours (six per probe) and the bake queue (Vulkan
        // against OpenGL: stage 5.6)
        if (!loaded) {
            throw std::runtime_error("light_probes needs a level");
        }
        const LightProbeCollection& lpc = sg->light_probe_collection;
        out["queue"] = (int)lpc.to_process.size();
        Json::Value probes(Json::arrayValue);
        for (const LightProbe& probe : lpc.light_probes) {
            Json::Value colors(Json::arrayValue);
            for (const vec3& c : probe.ambient_cube_color) {
                colors.append(c[0]);
                colors.append(c[1]);
                colors.append(c[2]);
            }
            probes.append(colors);
        }
        out["probes"] = probes;
    } else if (cmd == "texture_list") {
        std::vector<std::string> names;
        Textures::Instance()->ListNamed(names);
        Json::Value list(Json::arrayValue);
        for (const std::string& n : names) {
            list.append(n);
        }
        out["textures"] = list;
    } else if (cmd == "texture_dump") {
        // (a texture by its name: level 0 as raw floats, for comparing two renderers)
        int w = 0, h = 0;
        out["found"] = Textures::Instance()->DumpLevel(req.get("name", "").asString(), req.get("path", "").asString(), &w, &h);
        out["width"] = w;
        out["height"] = h;
    } else if (cmd == "texture_digest") {
        // Overgrowth Online (GL 4.6 stage 5.3): each texture loaded from a file, read
        // back and hashed by its name: OpenGL and Vulkan must agree (vk_textures)
        Json::Value list(Json::arrayValue);
        for (const Textures::Digest& d : Textures::Instance()->DigestLoaded()) {
            Json::Value v;
            v["name"] = d.name;
            v["width"] = d.width;
            v["height"] = d.height;
            v["layers"] = d.layers;
            v["format"] = (Json::UInt)d.format;
            char hex[24];
            FormatString(hex, sizeof(hex), "%016llx", (unsigned long long)d.hash);
            v["hash"] = hex;
            list.append(v);
        }
        out["textures"] = list;
        if (UseVulkan()) {
            vkgl::Stats s = vkgl::GetStats();
            Json::Value& vk = out["vulkan"];
            vk["images"] = (Json::UInt64)s.images;
            vk["image_mb"] = (double)s.image_bytes / (1024.0 * 1024.0);
            vk["buffers"] = (Json::UInt64)s.buffers;
            vk["buffer_mb"] = (double)s.buffer_bytes / (1024.0 * 1024.0);
            vk["samplers"] = (Json::UInt64)s.samplers;
            vk["staging_mb"] = (double)s.staging_bytes / (1024.0 * 1024.0);
            vk["uploads"] = (Json::UInt64)s.uploads;
            vk["upload_mb"] = (double)s.upload_bytes / (1024.0 * 1024.0);
        }
    } else if (cmd == "pointer") {
        const uint32_t previous = test_pointer_buttons;
        test_pointer_enabled = req.get("enabled", true).asBool();
        test_pointer_x = req.get("x", test_pointer_x).asInt();
        test_pointer_y = req.get("y", test_pointer_y).asInt();
        test_pointer_buttons = test_pointer_enabled ? req.get("buttons", 0).asUInt() : 0;
        const Uint32 window = SDL_GetWindowID(Graphics::Instance()->sdl_window_);
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_MOUSEMOTION;
        event.motion.windowID = window;
        event.motion.x = test_pointer_x;
        event.motion.y = test_pointer_y;
        SDL_PushEvent(&event);
        for (int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_RIGHT; ++button) {
            const uint32_t mask = SDL_BUTTON(button);
            if ((previous & mask) == (test_pointer_buttons & mask)) continue;
            SDL_zero(event);
            event.type = test_pointer_buttons & mask ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            event.button.windowID = window;
            event.button.button = button;
            event.button.state = test_pointer_buttons & mask ? SDL_PRESSED : SDL_RELEASED;
            event.button.x = test_pointer_x;
            event.button.y = test_pointer_y;
            SDL_PushEvent(&event);
        }
        out["enabled"] = test_pointer_enabled;
    } else if (cmd == "text") {
        const std::string text = req.get("text", "").asString();
        for (size_t offset = 0; offset < text.size();) {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_TEXTINPUT;
            event.text.windowID = SDL_GetWindowID(Graphics::Instance()->sdl_window_);
            size_t length = std::min(text.size() - offset, sizeof(event.text.text) - 1);
            while (offset + length < text.size() && length > 0 &&
                   (static_cast<unsigned char>(text[offset + length]) & 0xc0) == 0x80) --length;
            if (length == 0) throw std::runtime_error("invalid UTF-8 text input");
            std::memcpy(event.text.text, text.data() + offset, length);
            SDL_PushEvent(&event);
            offset += length;
        }
    } else if (cmd == "ui_state") {
        if (!ImGui::GetCurrentContext()) throw std::runtime_error("no ImGui context");
        const ImGuiContext& gui = *ImGui::GetCurrentContext();
        auto rectangle = [](const ImRect& rect) {
            Json::Value result(Json::arrayValue);
            result.append(rect.Min.x); result.append(rect.Min.y);
            result.append(rect.Max.x); result.append(rect.Max.y);
            return result;
        };
        out["active_id"] = gui.ActiveId;
        out["hovered_id"] = gui.HoveredId;
        out["nav_id"] = gui.NavId;
        out["nav_window"] = gui.NavWindow ? gui.NavWindow->Name : "";
        out["windowing_target"] = gui.NavWindowingTarget ? gui.NavWindowingTarget->Name : "";
        out["want_text_input"] = gui.IO.WantTextInput;
        if (gui.NavWindow) {
            ImRect rect = gui.NavWindow->NavRectRel[gui.NavLayer];
            rect.Translate(gui.NavWindow->Pos);
            out["nav_rect"] = rectangle(rect);
        }
        Json::Value windows(Json::arrayValue);
        for (const ImGuiWindow* window : gui.Windows) {
            if (!window->WasActive) continue;
            Json::Value row;
            row["name"] = window->Name;
            row["id"] = window->ID;
            row["rect"] = rectangle(window->Rect());
            row["clip_rect"] = rectangle(window->ClipRect);
            row["scroll_y"] = window->Scroll.y;
            row["max_scroll_y"] = window->ScrollMax.y;
            windows.append(row);
        }
        out["windows"] = windows;
    } else if (cmd == "key") {
        // A key pressed and let go as the keyboard sends it (SDL events through the engine's own
        // input path): {"key": "f1"}; optional "down" holds/releases across
        // frames for Dear ImGui, which reads the current key state.
        SDL_Scancode code = SDL_GetScancodeFromName(req.get("key", "").asString().c_str());
        out["ok"] = code != SDL_SCANCODE_UNKNOWN;
        if (code != SDL_SCANCODE_UNKNOWN) {
            Uint32 window = SDL_GetWindowID(Graphics::Instance()->sdl_window_);
            for (int i = 0; i < std::max(1, req.get("repeat", 1).asInt()); ++i) {
                SDL_Event e;
                SDL_zero(e);
                const bool held = req.get("down", true).asBool();
                e.type = held ? SDL_KEYDOWN : SDL_KEYUP;
                e.key.windowID = window;
                e.key.state = held ? SDL_PRESSED : SDL_RELEASED;
                e.key.keysym.scancode = code;
                e.key.keysym.sym = SDL_GetKeyFromScancode(code);
                SDL_PushEvent(&e);
                if (!req.isMember("down")) {
                    e.type = SDL_KEYUP;
                    e.key.state = SDL_RELEASED;
                    SDL_PushEvent(&e);
                }
            }
        }
    } else if (cmd == "screenshot") {
        Graphics* g = Graphics::Instance();
        g->queued_screenshot_path = req.get("path", "").asString();
        g->screenshot_mode = Graphics::kGameplay;
        g->pre_screenshot_media_mode_state = g->media_mode();
        g->queued_screenshot = 1;
    } else if (cmd == "menu") {
        // A menu script, as a menu button opens it ("multiplayer_menu.as")
        Engine::Instance()->ScriptableUICallback(req.get("script", "").asString());
    } else if (cmd == "forge") {
        // A guest's attack on the host, as a crafted message would be (test of the checks)
        std::string kind = req.get("kind", "").asString();
        int id = req.get("target", -1).asInt();
        const Json::Value& p = req["pos"];
        vec3 pos = p.isArray() ? vec3(p[0].asFloat(), p[1].asFloat(), p[2].asFloat()) : vec3(0.0f);
        if (kind == "malformed") {
            online->TestSendMalformed(req.get("mode", 0).asInt());
        } else if (kind == "remove_object") {
            online->Send<OnlineMessages::RemoveObject>(id, _movement_object);
        } else if (kind == "create_entity") {
            online->Send<OnlineMessages::CreateEntity>(req.get("path", "").asString(), pos, 999999);
        } else if (kind == "avatar_character") {
            online->Send<OnlineMessages::AvatarCharacter>(id, req.get("path", "").asString());
        } else if (kind == "owner_override") {
            online->Send<OnlineMessages::OwnerOverride>(id, 0, true, 1.0e6f);
        } else if (kind == "set_player_state") {
            PlayerState ps;
            ps.playername = "Hacked";
            ps.object_id = id;
            online->Send<OnlineMessages::SetPlayerState>((PlayerID)req.get("player", 0).asInt(), &ps);
        } else if (kind == "hit") {
            online->Send<OnlineMessages::HitRequest>(req.get("attacker", -1).asInt(), id, "attackimpact", "", vec3(0.0f, 0.0f, 1.0f), pos,
                                                     1000.0f, 1000.0f, 0.0f);
        } else if (kind == "teleport") {
            online->Send<OnlineMessages::EditorTransformChange>(id, pos, vec3(1.0f), quaternion());
        } else if (kind == "sp_string") {
            online->Send<OnlineMessages::SPStringMessage>(id, req.get("key", "Teams").asString(), req.get("value", "x").asString(),
                                                          ScriptParamEditorType::UNDEFINED, "");
        } else if (kind == "level") {
            online->Send<OnlineMessages::SendLevelMessage>(req.get("msg", "").asString());
        } else {
            throw std::runtime_error("forge: unknown kind");
        }
    } else if (cmd == "crash") {
        // Overgrowth Online: crash on purpose, to check the crash reports
        // (kind: thread, main, abort, stack); there is no reply
        LOGI << "test bridge: crashing (" << req.get("kind", "main").asString() << ")" << std::endl;
        CrashReportTestCrash(req.get("kind", "main").asString());
    } else if (cmd == "camera_rays") {
        // The camera's screen <-> world conventions (reversed-Z, stage 4a): the ray
        // through the screen's centre should be the facing; a point ahead projects to
        // the centre with depth in (0, 1]
        if (!loaded) {
            throw std::runtime_error("no level loaded");
        }
        Camera* cam = ActiveCameras::Get();
        Graphics* g = Graphics::Instance();
        auto v3 = [](const vec3& v) {
            Json::Value a(Json::arrayValue);
            a.append(v.x());
            a.append(v.y());
            a.append(v.z());
            return a;
        };
        vec3 eye = invert(cam->GetViewMatrix()).GetTranslationPart();
        out["facing"] = v3(cam->GetFacing());
        out["center_ray"] = v3(cam->GetRayThroughPixel(g->window_dims[0] / 2, g->window_dims[1] / 2));
        out["mouse_ray"] = v3(cam->GetMouseRay());
        out["ahead_10m"] = v3(cam->worldToScreen(eye + cam->GetFacing() * 10.0f));
        out["behind_10m"] = v3(cam->worldToScreen(eye - cam->GetFacing() * 10.0f));
        out["window"] = v3(vec3((float)g->window_dims[0], (float)g->window_dims[1], 0.0f));
    } else if (cmd == "flares") {
        // The lens flares this machine would draw now: [visible_mult, distant, visible, x, y, z]
        // each (the sun is one; an item's glint is "distant" only between glints, at 0;
        // visible = the occlusion query's share of the flare's quad seen; x, y, z its
        // position, a direction for a distant one)
        out["flares"] = Json::Value(Json::arrayValue);
        if (loaded) {
            for (Flare* f : sg->flares.flares) {
                Json::Value e(Json::arrayValue);
                e.append(f->visible_mult);
                e.append(f->distant);
                e.append(f->visible);
                for (int i = 0; i < 3; ++i) {
                    e.append(f->position[i]);
                }
                out["flares"].append(e);
            }
        }
    } else if (cmd == "net_nan") {
        // Our characters' records go out non-finite for this long (seconds)
        online->test_nan_stream_until_ms = OnlineClockMs() + req.get("seconds", 2.0).asDouble() * 1000.0;
    } else if (cmd == "net_sim" && req.isMember("conn_id")) {
        int c = req["conn_id"].asInt() & 255;
        online->net_sim_conn_latency_ms[c] = req.get("latency_ms", 0).asInt();
        online->net_sim_conn_jitter_ms[c] = req.get("jitter_ms", 0).asInt();
    } else if (cmd == "net_sim") {
        online->net_sim_latency_ms = req.get("latency_ms", 0).asInt();
        online->net_sim_jitter_ms = req.get("jitter_ms", 0).asInt();
        online->net_sim_loss = req.get("loss", 0.0).asFloat();
    } else if (cmd == "stick_item") {
        // Host: a free item sticks in a character (item=, char= local ids) as a
        // thrown one does when it hits: here, then everyone (ItemHit), and the
        // character's owner takes the hit
        if (!loaded || online->IsClient()) {
            throw std::runtime_error("stick_item needs the host");
        }
        Object* io = sg->GetObjectFromID(req.get("item", -1).asInt());
        Object* co = sg->GetObjectFromID(req.get("char", -1).asInt());
        if (io == nullptr || co == nullptr || io->GetType() != _item_object || co->GetType() != _movement_object) {
            throw std::runtime_error("stick_item: no such item or character");
        }
        ItemObject* item = static_cast<ItemObject*>(io);
        MovementObject* mo = static_cast<MovementObject*>(co);
        if (item->IsHeld() || item->StuckInWhom() != -1) {
            throw std::runtime_error("stick_item: the item is not free");
        }
        vec3 at = mo->position + vec3(0.0f, 0.15f, 0.0f);
        vec3 start = at + vec3(0.9f, 0.0f, 0.0f), end = at - vec3(0.9f, 0.0f, 0.0f);
        mat4 t = item->GetPhysicsTransform();
        t.SetTranslationPart(at);
        int before = (int)mo->rigged_object()->stuck_items.items.size();
        mo->rigged_object()->StickItem(item->GetID(), start, end, t);
        out["stuck"] = (int)mo->rigged_object()->stuck_items.items.size() > before;
        if (out["stuck"].asBool()) {
            online->SendItemHit(mo->GetID(), item->GetID(), at, "flesh", 2, vec3(-8.0f, 0.0f, 0.0f), start, end, t);
            if (online->IsLocalAvatar(mo->GetID())) {
                mo->HitByItem(item->GetID(), at, "flesh", 2);
            }
        }
    } else if (cmd == "armor_fixture") {
        // Opt-in integration fixtures: attach an exact stock wearable socket,
        // or reproduce a duplicated runtime mesh without bypassing reconciliation.
        MovementObject* actor = loaded ? FindCharacter(sg, req.get("char", -1).asInt()) : nullptr;
        if (!actor) throw std::runtime_error("armor_fixture needs a character");
        const std::string op = req.get("op", "").asString();
        if (op == "wear") {
            if (online->IsClient()) throw std::runtime_error("armor wearable needs host authority");
            Object* object = sg->GetObjectFromID(req.get("item", -1).asInt());
            const auto socket = engine->GetAssetManager()->LoadSync<Attachment>(req["slot"].asString());
            if (!object || object->GetType() != _item_object || !socket.valid())
                throw std::runtime_error("armor_fixture needs an item and valid socket");
            actor->AttachItemToSlotEditor(object->GetID(), _at_attachment, req.get("mirrored", false).asBool(), socket, false);
        } else if (op == "detach") {
            if (online->IsClient()) throw std::runtime_error("armor detach needs host authority");
            const int item = req.get("item", -1).asInt();
            actor->NetworkDetachItem(item); // retain the editor restoration link
            online->Send<OnlineMessages::AttachToMessage>(actor->GetID(), item, 0, false, false, nullptr, vec3(1), 0, true);
        } else if (op == "duplicate") {
            if (online->IsClient()) throw std::runtime_error("armor duplicate needs host authority");
            auto* rig = actor->rigged_object();
            const ObjectID source = req.get("piece", -1).asInt();
            auto found = std::find_if(rig->children.begin(), rig->children.end(), [source](const AttachedEnvObject& a) {
                return a.direct_ptr && a.direct_ptr->GetID() == source;
            });
            if (found == rig->children.end()) throw std::runtime_error("armor fixture source is not attached");
            const AttachedEnvObject original = *found;
            Object* piece = original.direct_ptr;
            const int region = piece->GetScriptParams()->HasParam("Campaign Armor Region") ?
                piece->GetScriptParams()->ASGetInt("Campaign Armor Region") : req.get("region", 0).asInt();
            if (region <= 0 || region > CampaignEquipment::kAllRegions || (region & (region - 1)))
                throw std::runtime_error("armor fixture needs a single armor region");
            const std::string path = req.get("path", piece->obj_file).asString();
            const ObjectID copy_id = sg->map_editor->CreateObject(path);
            Object* copy = sg->GetObjectFromID(copy_id);
            if (!copy || copy->GetType() != _env_object) throw std::runtime_error("armor fixture failed to duplicate mesh");
            for (const auto& param : piece->GetScriptParams()->GetParameterMap()) copy->GetScriptParams()->InsertScriptParam(param.first, param.second);
            const bool authored = req.get("authored", false).asBool();
            if (authored) {
                copy->GetScriptParams()->ASRemove("Campaign Armor Region");
                copy->GetScriptParams()->ASRemove("Progression Armor Region");
                copy->GetScriptParams()->ASRemove("Armor Material Tier");
                copy->GetScriptParams()->ASRemove("Armor Slot Suppressed");
            }
            copy->exclude_from_save = copy->exclude_from_undo = true;
            copy->SetTranslation(piece->GetTranslation()); copy->SetRotation(piece->GetRotation()); copy->SetScale(piece->GetScale());
            copy->SetEnabled(true);
            copy->SetParent(actor); copy->SetCollisionEnabled(false);
            auto duplicate = original;
            duplicate.direct_ptr = copy; duplicate.legacy_obj_id = copy_id;
            rig->children.push_back(duplicate);
            online->Send<OnlineMessages::CreateEntity>(copy->obj_file, copy->GetTranslation(), copy_id);
            online->Send<OnlineMessages::AttachToMessage>(actor->GetID(), copy_id, 0, true, false, &duplicate,
                                                          copy->GetScale(), authored ? 0 : region);
            out["copy"] = copy_id;
            out["region"] = region;
            int copies = 0;
            for (const auto& child : rig->children)
                if (child.direct_ptr && child.direct_ptr->GetScriptParams()->HasParam("Campaign Armor Region") &&
                    child.direct_ptr->GetScriptParams()->ASGetInt("Campaign Armor Region") == region) ++copies;
            out["generated_copies"] = copies;
        } else if (op != "inspect") throw std::runtime_error("unknown armor fixture operation");
        out["authored_regions"] = online->AuthoredArmorRegions(actor->GetID());
        out["rating"] = online->WornArmorRating(actor->GetID());
    } else if (cmd == "spawn") {
        // Host (or offline): a character from an object file, replicated to
        // the clients the way the engine adds a hot-joining player
        if (!loaded || online->IsClient()) {
            throw std::runtime_error("spawn needs the host");
        }
        std::string path = req.get("path", "Data/Objects/IGF_Characters/IGF_GuardActor.xml").asString();
        const Json::Value& p = req["pos"];
        vec3 pos(p[0].asFloat(), p[1].asFloat(), p[2].asFloat());
        int id = sg->map_editor->CreateObject(path);
        Object* obj = sg->GetObjectFromID(id);
        if (obj == nullptr) {
            throw std::runtime_error("spawn failed: " + path);
        }
        obj->SetTranslation(pos);
        MovementObject* mo = FindCharacter(sg, id);
        if (mo != nullptr && req.isMember("teams")) {
            mo->GetScriptParams()->ASSetString("Teams", req["teams"].asString());
        }
        if (online->IsActive()) {
            online->Send<OnlineMessages::CreateEntity>(path, pos, id);
        }
        out["object_id"] = id;  // ("id" is the request's)
    } else if (cmd == "player_pose_fault") {
        // Opt-in integration fault injection; never touches a remote actor or
        // ordinary NPC. Keep the source tick delayed so menu recovery, rather
        // than the next normal character update, must initialize the pose.
        MovementObject* actor = loaded ? FindCharacter(sg, req.get("char", -1).asInt()) : nullptr;
        Input* input = Input::Instance();
        if (!actor || !actor->controlled || !actor->as_context || !actor->rigged_object() ||
            input->LocalPlayerIndexForInputID(actor->controller_id) < 0 ||
            (online->IsActive() && !online->OwnPlayerCharacter(actor->GetID())))
            throw std::runtime_error("pose fault requires an owned local human");
        const auto common = online->GetOriginalID(actor->GetID());
        auto describe = [&]() {
            Json::Value state(Json::objectValue);
            state["script_counter"] = actor->update_script_counter;
            state["proxy"] = actor->network_proxy;
            state["static"] = actor->static_char;
            state["hold"] = actor->ASHasVar("coop_cutscene_hold") && actor->ASGetBoolVar("coop_cutscene_hold");
            state["dialogue"] = actor->ASHasVar("dialogue_control") && actor->ASGetBoolVar("dialogue_control");
            state["animation"] = actor->rigged_object()->anim_client.GetCurrAnim();
            const auto owner = online->owner_overrides.find(common);
            state["owner"] = owner == online->owner_overrides.end() ? -1 : static_cast<int>(owner->second.owner);
            return state;
        };
        out["before"] = describe();
        const bool restoring = req["restore"].isObject();
        const auto& fault = restoring ? req["restore"] : req;
        const int owner = fault.get("owner", out["before"]["owner"]).asInt();
        if (owner < -1 || owner > 255 || (owner >= 0 && !online->IsActive()))
            throw std::runtime_error("invalid pose fault owner");
        if (owner >= 0) {
            Online::OwnerOverrideEntry replacement;
            replacement.owner = static_cast<PlayerID>(owner);
            replacement.since_ms = OnlineClockMs();
            replacement.max_ms = 10000.0;
            online->owner_overrides[common] = replacement;
        } else online->owner_overrides.erase(common);
        actor->static_char = fault.get("static", actor->static_char).asBool();
        if (actor->ASHasVar("coop_cutscene_hold")) actor->ASSetBoolVar("coop_cutscene_hold", fault.get("hold", out["before"]["hold"]).asBool());
        if (actor->ASHasVar("dialogue_control")) actor->ASSetBoolVar("dialogue_control", fault.get("dialogue", out["before"]["dialogue"]).asBool());
        actor->network_proxy = fault.get("proxy", actor->network_proxy).asBool();
        actor->rigged_object()->network_proxy = actor->network_proxy;
        actor->update_script_counter = std::max(-1200, std::min(1200, fault.get("script_counter", restoring ? actor->update_script_counter : 240).asInt()));
        if (!restoring && fault.get("clear_animation", true).asBool()) actor->rigged_object()->anim_client.Reset();
        out["actor"] = actor->GetID();
        out["after"] = describe();
    } else if (cmd == "rig_check") {
        // How well each bone's joint with its parent predicts it right now
        // (the snapshot rig, Online/char_snapshot.h), in mm
        MovementObject* mo = loaded ? FindCharacter(sg, req.get("char", -1).asInt()) : nullptr;
        if (mo == nullptr) {
            throw std::runtime_error("no such character");
        }
        RiggedObject* rig_obj = mo->rigged_object();
        const CharSnapshot::Rig& rig = rig_obj->NetworkRig();
        const std::vector<PhysicsBone>& pb = rig_obj->skeleton_.physics_bones;
        out["bones"] = (int)pb.size();
        out["parents"] = (int)rig_obj->skeleton_.parents.size();
        out["points"] = (int)rig_obj->skeleton_.points.size();
        out["errors_mm"] = Json::Value(Json::arrayValue);
        int linked = 0;

        for (size_t i = 0; i < pb.size() && i < rig.links.size(); ++i) {
            const CharSnapshot::BoneLink& link = rig.links[i];
            if (link.parent < 0 || pb[i].bullet_object == nullptr || pb[link.parent].bullet_object == nullptr) {
                continue;
            }
            ++linked;
            const mat4& tp = pb[link.parent].bullet_object->transform;
            const mat4& tc = pb[i].bullet_object->transform;
            vec3 predicted = CharSnapshot::PredictFromParent(link, QuaternionFromMat4(tp.GetRotationPart()), tp.GetTranslationPart(),
                                                             QuaternionFromMat4(tc.GetRotationPart()));
            Json::Value e(Json::arrayValue);
            e.append((int)i);
            e.append(link.parent);
            e.append(distance(predicted, tc.GetTranslationPart()) * 1000.0f);
            e.append(length(tc.GetTranslationPart() - tp.GetTranslationPart()) * 1000.0f);
            e.append(length(link.joint_in_parent - link.joint_in_child) * 1000.0f);
            out["errors_mm"].append(e);
        }
        out["linked"] = linked;
        out["ends"] = Json::Value(Json::arrayValue);
        for (const PhysicsBone& b : pb) {
            Json::Value e(Json::arrayValue);
            e.append(b.bone);
            if (b.bone >= 0 && b.bone < (int)rig_obj->skeleton_.bones.size()) {
                e.append(rig_obj->skeleton_.bones[b.bone].points[0]);
                e.append(rig_obj->skeleton_.bones[b.bone].points[1]);
            }
            e.append(rig_obj->skeleton_.parents.size() > (size_t)b.bone ? rig_obj->skeleton_.parents[b.bone] : -2);
            e.append(b.bullet_object != nullptr);
            out["ends"].append(e);
        }
    } else if (cmd == "nan_scan") {
        // Objects with a non-finite transform; characters with non-finite bones
        auto bad3 = [](const vec3& v) { return !std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]); };
        auto bad_mats = [](const std::vector<mat4>& mats) {
            for (const mat4& m : mats) {
                for (int i = 0; i < 16; ++i) {
                    if (!std::isfinite(m.entries[i])) {
                        return true;
                    }
                }
            }
            return false;
        };
        out["objects"] = Json::Value(Json::arrayValue);
        out["characters"] = Json::Value(Json::arrayValue);
        out["cameras"] = Json::Value(Json::arrayValue);
        if (loaded) {
            for (Object* obj : sg->objects_) {
                bool transform = false;
                for (int element = 0; element < 16; ++element)
                    transform = transform || !std::isfinite(obj->GetTransform().entries[element]);
                if (bad3(obj->GetTranslation()) || bad3(obj->GetScale()) || transform) {
                    Json::Value e;
                    e["id"] = obj->GetID();
                    e["type"] = (int)obj->GetType();
                    e["path"] = obj->obj_file;
                    e["transform"] = transform;
                    out["objects"].append(e);
                }
            }
            for (int id = 0; id < ActiveCameras::NumCameras(); ++id) {
                Camera* camera = ActiveCameras::GetCamera(id);
                if (!camera) continue;
                const mat4 view = camera->GetViewMatrix(), projection = camera->GetProjMatrix();
                bool bad = bad3(camera->GetPos());
                for (int element = 0; element < 16; ++element)
                    bad = bad || !std::isfinite(view.entries[element]) || !std::isfinite(projection.entries[element]);
                if (bad) out["cameras"].append(id);
            }
            for (Object* obj : sg->movement_objects_) {
                MovementObject* mo = (MovementObject*)obj;
                RiggedObject* r = mo->rigged_object();
                bool pos = bad3(mo->position);
                bool disp = bad_mats(r->display_bone_matrices);
                bool net = bad_mats(r->network_display_bone_matrices);
                bool phys = false;
                for (const PhysicsBone& b : r->skeleton_.physics_bones) {
                    phys = phys || (b.bullet_object != nullptr && bad3(b.bullet_object->transform.GetTranslationPart()));
                }
                bool points = false;
                for (const vec3& p : r->skeleton_.points) {
                    points = points || bad3(p);
                }
                if (pos || disp || net || phys || points) {
                    Json::Value e;
                    e["id"] = mo->GetID();
                    e["proxy"] = mo->network_proxy;
                    e["rig_proxy"] = r->network_proxy;
                    e["position"] = pos;
                    e["display"] = disp;
                    e["network"] = net;
                    e["physics"] = phys;
                    e["points"] = points;
                    e["display_n"] = (int)r->display_bone_matrices.size();
                    e["network_n"] = (int)r->network_display_bone_matrices.size();
                    // which physics bones, and the rig links that rebuild them
                    const CharSnapshot::Rig& rig = r->network_rig;
                    e["nan_bones"] = Json::Value(Json::arrayValue);
                    for (size_t i = 0; i < r->skeleton_.physics_bones.size(); ++i) {
                        const BulletObject* b_o = r->skeleton_.physics_bones[i].bullet_object;
                        if (b_o != nullptr && bad3(b_o->transform.GetTranslationPart())) {
                            Json::Value nb(Json::arrayValue);
                            nb.append((int)i);
                            bool has_link = i < rig.links.size();
                            nb.append(has_link ? rig.links[i].parent : -9);
                            nb.append(has_link && (bad3(rig.links[i].joint_in_parent) || bad3(rig.links[i].joint_in_child)));
                            nb.append(bad3(r->skeleton_.physics_bones[i].initial_position));
                            e["nan_bones"].append(nb);
                        }
                    }
                    e["rig_scale"] = r->network_rig_scale;
                    e["model_scale"] = r->GetCharScale();
                    out["characters"].append(e);
                }
            }
        }
    } else if (cmd == "build") {
        // Overgrowth Online: building together. {"on": bool} build mode (host), {"builder": id,
        // "may": bool} (host), {"editor": bool} opens or closes the editor here; the answer: the state
        Online* online = Online::Instance();
        if (req.isMember("on")) {
            online->SetBuildSession(req["on"].asBool());  // (a build session: building allowed)
        }
        if (req.isMember("freeze")) {
            online->SetTimeFrozen(req["freeze"].asBool());
        }
        if (req.get("restart", false).asBool()) {
            online->RestartLevel(Engine::Instance()->GetSceneGraph());
        }
        if (req.isMember("builder")) {
            online->SetBuilder(req["builder"].asInt(), req.get("may", true).asBool());
        }
        if (req.isMember("editor")) {
            out["editor_ok"] = Engine::Instance()->SetEditorOpen(req["editor"].asBool());
        }
        if (req.get("f1", false).asBool()) {
            Engine::Instance()->PressEditorKey();  // (F1 as the player presses it: building follows the editor)
        }
        if (req.get("autosave", false).asBool()) {
            online->AutosaveSoon();
        }
        if (req.get("build_on_load", false).asBool()) {
            online->BuildOnLoad();  // (Build a level: before "host")
        }
        if (req.isMember("builders_save")) {
            online->builders_may_save = req["builders_save"].asBool();
        }
        SceneGraph* bsg = Engine::Instance()->GetSceneGraph();
        if (req.isMember("save_to") && bsg != nullptr && bsg->map_editor != nullptr) {
            // (host: the level made a file in the write folder, then saved as the editor does)
            out["save_to_ok"] = LevelLoader::PointLevelAt(*bsg, req["save_to"].asString());
            if (out["save_to_ok"].asBool()) {
                bsg->map_editor->SaveLevel(LevelLoader::kSaveInPlace);
            }
        }
        if (req.get("save", false).asBool() && bsg != nullptr && bsg->map_editor != nullptr) {
            bsg->map_editor->SaveLevel(LevelLoader::kSaveInPlace);  // (a builder: asks the host)
        }
        if (req.isMember("answer_save")) {
            online->AnswerSaveRequest(req["answer_save"].asBool());
        }
        out["save_state"] = online->SaveStateText();
        out["saved_ago"] = online->SavedAgo();
        out["autosaved_ago"] = online->AutosavedAgo();
        out["unsaved"] = online->UnsavedCount();
        out["save_request"] = online->SaveRequestFrom();
        out["nav_builds"] = online->nav_builds;
        out["nav_build_ms"] = online->nav_build_ms;
        out["refl_refreshes"] = online->refl_refreshes;
        out["level_name"] = online->level_name;
        out["level_files_sent"] = online->level_files_sent;
        out["level_files_received"] = online->level_files_received;
        out["everyone_builds"] = online->everyone_builds;
        out["build_session"] = online->BuildSession();
        out["banner"] = online->SessionBanner();
        out["saved_levels"] = Json::Value(Json::arrayValue);
        for (const std::string& level : Online::SavedLevels()) {
            out["saved_levels"].append(level);
        }
        out["refl_captures"] = online->refl_last_count;
        int refl_dirty = 0;
        if (bsg != nullptr) {
            for (Object* obj : bsg->objects_) {
                if (obj->GetType() == _reflection_capture_object && static_cast<ReflectionCaptureObject*>(obj)->dirty) {
                    ++refl_dirty;  // (marked, not rendered yet)
                }
            }
        }
        out["refl_dirty"] = refl_dirty;
        out["chat"] = Json::Value(Json::arrayValue);
        for (const std::string& line : online->chat_log) {
            out["chat"].append(line);
        }
        out["build_mode"] = online->BuildSession();  // (a build session)
        out["time_frozen"] = online->TimeFrozen();
        out["batches_sent"] = online->edit_sync.batches_sent;
        out["may_edit"] = online->MayUseEditor();
        out["editor_open"] = Engine::Instance()->current_engine_state_.type == kEngineEditorLevelState;
        out["scans"] = online->edit_sync.scans;
        out["sent"] = online->edit_sync.batches_sent;
        out["applied"] = online->edit_sync.batches_applied;
        out["refused"] = online->edit_sync.refused;
        out["edited"] = online->edit_sync.edited_count();
        out["presence"] = Json::Value(Json::objectValue);
        for (const auto& p : online->presence_) {
            Json::Value v;
            v["editing"] = p.second.editing;
            v["camera"].append(p.second.camera[0]);
            v["camera"].append(p.second.camera[1]);
            v["camera"].append(p.second.camera[2]);
            v["selection"] = Json::Value(Json::arrayValue);
            for (int id : p.second.selection) {
                v["selection"].append(id);
            }
            out["presence"][std::to_string(p.first)] = v;
        }
        out["claims"] = Json::Value(Json::objectValue);
        for (const auto& c : online->claims_) {
            out["claims"][std::to_string(c.first)] = c.second;
        }
    } else if (cmd == "edit") {
        // Overgrowth Online: editor operations as the editor's tools do them, for the building
        // tests. {"op": "spawn", "path", "pos"} | {"op": "move", "obj", "pos"} | {"op": "delete", "obj"}
        // | {"op": "param", "obj", "key", "value"} | {"op": "group", "objs"} | {"op": "get", "obj"} |
        // {"op": "find", "path"}. Ids are this machine's ("obj": "id" is the bridge's own request id);
        // answers carry each object's shared id too
        Online* online = Online::Instance();
        SceneGraph* sg = Engine::Instance()->GetSceneGraph();
        if (sg == nullptr || sg->map_editor == nullptr) {
            throw std::runtime_error("no level loaded");
        }
        std::string op = req.get("op", "").asString();
        auto vec = [](const Json::Value& v) { return vec3(v[0].asFloat(), v[1].asFloat(), v[2].asFloat()); };
        auto describe = [&](Object* obj) {
            Json::Value o;
            o["id"] = obj->GetID();
            o["shared"] = (int)online->GetOriginalID(obj->GetID());
            vec3 pos = obj->GetTranslation();
            o["pos"].append(pos[0]);
            o["pos"].append(pos[1]);
            o["pos"].append(pos[2]);
            o["file"] = obj->obj_file;
            o["type"] = CStringFromEntityType(obj->GetType());
            o["parent"] = obj->parent ? obj->parent->GetID() : -1;
            o["selected"] = obj->Selected();
            return o;
        };
        if (op == "spawn") {
            EntityDescriptionList descs;
            std::string file_type;
            Path source;
            ActorsEditor_LoadEntitiesFromFile(req["path"].asString(), descs, &file_type, &source);
            std::vector<Object*> made = ActorsEditor_AddEntitiesAtPosition(source, sg, descs, vec(req["pos"]), false);
            for (Object* obj : made) {
                out["made"].append(describe(obj));
            }
            sg->map_editor->QueueSaveHistoryState();
        } else if (op == "find") {
            out["objects"] = Json::Value(Json::arrayValue);
            for (Object* obj : sg->objects_) {
                if (obj->obj_file == req["path"].asString()) {
                    out["objects"].append(describe(obj));
                }
            }
        } else {
            int id = req.get("obj", -1).asInt();
            Object* obj = (id >= 0 && sg->DoesObjectWithIdExist(id)) ? sg->GetObjectFromID(id) : nullptr;
            if (op == "get") {
                out["exists"] = obj != nullptr;
                if (obj) {
                    out["object"] = describe(obj);
                    if (req.isMember("key") && obj->GetScriptParams()->HasParam(req["key"].asString())) {
                        out["value"] = obj->GetScriptParams()->GetStringVal(req["key"].asString());
                    }
                }
            } else if (op == "undo") {
                sg->map_editor->UndoHere();
            } else if (op == "redo") {
                sg->map_editor->RedoHere();
            } else if (op == "select") {
                MapEditor::DeselectAll(sg);
                for (const Json::Value& v : req["objs"]) {
                    if (sg->DoesObjectWithIdExist(v.asInt())) {
                        sg->GetObjectFromID(v.asInt())->Select(true);
                    }
                }
            } else if (op == "group") {
                MapEditor::DeselectAll(sg);
                for (const Json::Value& v : req["objs"]) {
                    if (sg->DoesObjectWithIdExist(v.asInt())) {
                        sg->GetObjectFromID(v.asInt())->Select(true);
                    }
                }
                sg->map_editor->GroupSelected();
                MapEditor::DeselectAll(sg);
            } else if (obj == nullptr) {
                throw std::runtime_error("no object " + std::to_string(id));
            } else if (op == "preview") {
                // (a frame of a drag, as the others see it: the live preview only, nothing moved
                // here and no editor operation)
                online->Send<OnlineMessages::EditorTransformChange>(id, vec(req["pos"]), obj->GetScale(), obj->GetRotation());
            } else if (op == "move") {
                obj->SetTranslation(vec(req["pos"]));
                obj->HandleTransformationOccurred();
                sg->map_editor->QueueSaveHistoryState();
            } else if (op == "delete") {
                sg->map_editor->RemoveObject(obj, sg);
                sg->map_editor->QueueSaveHistoryState();
            } else if (op == "param") {
                ScriptParamMap spm = obj->GetScriptParamMap();
                ScriptParam param;
                param.SetString(req["value"].asString());
                spm[req["key"].asString()] = param;
                sg->map_editor->ApplyScriptParams(spm, id);
            } else {
                throw std::runtime_error("unknown op " + op);
            }
        }
    } else if (cmd == "level_messages") {
        // Level messages seen since the last call (filter by prefix)
        std::string prefix = req.get("prefix", "").asString();
        out["messages"] = Json::Value(Json::arrayValue);
        for (const std::string& msg : level_messages_) {
            if (msg.compare(0, prefix.size(), prefix) == 0) {
                out["messages"].append(msg);
            }
        }
        level_messages_.clear();
    } else if (cmd == "track") {
        // Record these characters every step (clears the previous recording)
        online->track_samples.clear();
        for (const Json::Value& id : req["ids"]) {
            online->track_samples[id.asInt()];
        }
    } else if (cmd == "track_get") {
        Json::Value& t = out["tracks"];
        t = Json::Value(Json::objectValue);

        for (const auto& it : online->track_samples) {
            Json::Value rows(Json::arrayValue);
            for (const Online::TrackSample& s : it.second) {
                Json::Value row(Json::arrayValue);
                row.append(s.clock_ms);
                row.append(s.render_src_ms);
                for (int i = 0; i < 3; ++i) {
                    row.append(s.position[i]);
                }
                for (int i = 0; i < 3; ++i) {
                    row.append(s.bones[i]);
                }
                rows.append(row);
            }
            t[std::to_string(it.first)] = rows;
        }

        if (req.get("stop", false).asBool()) {
            online->track_samples.clear();
        }
    } else if (cmd == "quit") {
        engine->quitting_ = true;
    } else {
        throw std::runtime_error("unknown command: " + cmd);
    }
}
