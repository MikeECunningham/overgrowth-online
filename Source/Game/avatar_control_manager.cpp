//-----------------------------------------------------------------------------
//           Name: avatar_control_manager.cpp
//      Developer: Wolfire Games LLC
//    Modified By: Overgrowth Online (crxhvrd)
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
//
//   Copyright 2022 Wolfire Games LLC
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//-----------------------------------------------------------------------------
#include "avatar_control_manager.h"

#include <Main/engine.h>
#include <Main/scenegraph.h>

#include <Graphics/camera.h>
#include <Editors/map_editor.h>
#include <Objects/cameraobject.h>
#include <Game/level.h>
#include <Online/online.h>
#include <UserInput/input.h>

AvatarControlManager::RemoteSlot AvatarControlManager::ClientRemoteSlot(ObjectID avatar) {
    RemoteSlot& slot = client_remote_slots_[avatar];

    if (slot.controller_id == -1) {
        slot.controller_id = Input::Instance()->AllocateRemotePlayerInput();
        slot.camera_id = ActiveCameras::Instance()->CreateVirtualCameraInstance();
    }

    return slot;
}

std::vector<Possession> AvatarControlManager::GeneratePossessionList() {
    Engine* engine = Engine::Instance();
    Online* online = Online::Instance();
    SceneGraph* scenegraph_ = Engine::Instance()->GetSceneGraph();

    std::vector<Possession> possessions;

    if (!online->IsActive()) {
        // Offline player actors own contiguous local input/camera slots. The
        // dynamic scene query preserves the order of already assigned players.
        possessions.reserve(engine->avatar_ids.size());
        for (size_t i = 0; i < engine->avatar_ids.size(); ++i) {
            const int controller_id = static_cast<int>(i);
            possessions.emplace_back(engine->avatar_ids[i], controller_id,
                                     engine->GetSplitScreen() ? controller_id : 0, true);
        }
        return possessions;
    }

    int local_player_index = 0;
    if (online->IsActive()) {
        for (auto& player_state : online->GetPlayerStates()) {
            if (player_state.object_id == -1) {
                continue;
            }

            bool is_local = player_state.peer_id == online->LocalPeerID();
            int camera_id = -1;
            int controller_id = -1;

            if (is_local) {
                camera_id = std::max(0, player_state.screen - 1);
                controller_id = Input::Instance()->GetLocalPlayerInputID(player_state.local_index);
                const int previous_camera = ActiveCameras::GetID();
                ActiveCameras::Set(camera_id);
                ActiveCameras::Set(previous_camera);
                local_player_index++;
            } else if (online->IsClient()) {
                // Another player's avatar on a client: our own virtual slot
                RemoteSlot slot = ClientRemoteSlot(player_state.object_id);
                camera_id = slot.camera_id;
                controller_id = slot.controller_id;
            } else {
                // Virtual controller_id, used for syncing input from remote source
                camera_id = player_state.camera_id;
                controller_id = player_state.controller_id;
            }

            MovementObject* mo = nullptr;
            if (scenegraph_ != nullptr) {
                Object* ob = scenegraph_->GetObjectFromID(player_state.object_id);

                if (ob != nullptr && ob->GetType() == EntityType::_movement_object) {
                    mo = static_cast<MovementObject*>(ob);
                }
            }

            if (mo != nullptr && camera_id != -1 && controller_id != -1) {
                possessions.push_back(Possession(
                    player_state.object_id,
                    controller_id,
                    camera_id,
                    is_local));
            }
        }
    }

    std::vector<ObjectID> unpossessed_avatars = GetUnpossessedAvatars();

    for (int i = 0; i < unpossessed_avatars.size(); i++) {
        if (online->IsClient()) {
            // Not ours and no player's: the host runs it, keep it off our input and camera
            RemoteSlot slot = ClientRemoteSlot(unpossessed_avatars[i]);
            possessions.push_back(Possession(unpossessed_avatars[i], slot.controller_id, slot.camera_id, false));
            continue;
        }

        // The host leaves unclaimed map avatars available for roster players;
        // they are not additional people on the host's keyboard.
        if (online->IsHosting()) continue;

        int camera_id = 0;

        if (Engine::Instance()->GetSplitScreen()) {
            camera_id = i;
        }

        possessions.push_back(Possession(unpossessed_avatars[i], local_player_index, camera_id, !Online::Instance()->IsActive() || Online::Instance()->IsHosting()));
        local_player_index++;
    }

    return possessions;
}

std::vector<ObjectID> AvatarControlManager::GetUnpossessedAvatars() {
    Online* online = Online::Instance();
    SceneGraph* scenegraph_ = Engine::Instance()->GetSceneGraph();

    std::vector<ObjectID> unpossessed_avatars;

    std::vector<int> avatar_ids;
    if (scenegraph_ != nullptr) {
        scenegraph_->GetPlayerCharacterIDs(avatar_ids);

        for (int avatar_id : avatar_ids) {
            bool found = false;
            for (const auto& state : online->GetPlayerStates()) found = found || state.object_id == avatar_id;

            if (found == false) {
                unpossessed_avatars.push_back(avatar_id);
            }
        }
    }

    return unpossessed_avatars;
}

void AvatarControlManager::Update() {
    Engine* engine = Engine::Instance();
    Online* online = Online::Instance();
    SceneGraph* scenegraph_ = Engine::Instance()->GetSceneGraph();

    scenegraph_->GetPlayerCharacterIDs(engine->avatar_ids);
    engine->num_avatars = static_cast<int>(engine->avatar_ids.size());

    if (scenegraph_->map_editor->state_ != MapEditor::kInGame) {  // We probably shouldn't do this in mp at all, not even our own controlable char as host
        for (int i = 0; i < engine->num_avatars; ++i) {
            Object* obj = scenegraph_->GetObjectFromID(engine->avatar_ids[i]);
            if (obj->GetType() == _movement_object) {
                MovementObject* avatar = (MovementObject*)obj;

                // If we're in a multiplayer game, don't switch other players characters to NPC mode, only do it to ours.
                // (Overgrowth Online: a guest's too: the others' there run on the host's stream)
                if (!online->IsActive() || online->IsLocalAvatar(engine->avatar_ids[i])) {
                    avatar->ChangeControlScript(scenegraph_->level->GetNPCScript(avatar));
                    avatar->controlled = false;
                    avatar->camera_id = 0;
                }
            }
        }
        // Overgrowth Online: our own avatar in a session, is_player here or not. A guest's comes from
        // the host's player states: in a build session its editor opened over a character that kept
        // its player script, camera and the mouse (the view jumped between the editor's camera and
        // the character's, and the character still ran)
        if (online->IsActive()) {
            for (auto& player_state : online->GetPlayerStates()) {
                if (player_state.object_id == -1 || !online->IsLocalAvatar(player_state.object_id)) {
                    continue;
                }
                Object* obj = scenegraph_->GetObjectFromID(player_state.object_id);
                if (obj != nullptr && obj->GetType() == _movement_object) {
                    MovementObject* avatar = (MovementObject*)obj;
                    avatar->ChangeControlScript(scenegraph_->level->GetNPCScript(avatar));
                    avatar->controlled = false;
                    avatar->camera_id = 0;
                }
            }
        }
        // Only the primary camera has the level's editor camera object. Local
        // chase cameras are created dynamically and do not own editor objects.
        Camera* editor_camera = ActiveCameras::GetCamera(0);
        CameraObject* camera_object = editor_camera ? editor_camera->getCameraObject() : nullptr;
        if (camera_object != nullptr && !camera_object->controlled) {
            camera_object->controlled = true;
            camera_object->SetTranslation(editor_camera->GetPos());
        }
    } else {
        // If there is no controllable avatars in the scene, create one at the camera position and use it.
        // Overgrowth Online: not on a client, its character comes from the host (arenas have none at first)
        if (engine->num_avatars == 0 && !online->IsClient()) {
            int id = scenegraph_->map_editor->CreateObject("Data/Objects/IGF_Characters/IGF_TurnerActor.xml");
            MovementObject* mo = static_cast<MovementObject*>(scenegraph_->GetObjectFromID(id));
            Camera* camera = ActiveCameras::GetCamera(0);
            CameraObject* camera_object = camera ? camera->getCameraObject() : nullptr;
            mo->SetTranslation(camera_object ? camera_object->GetTranslation() :
                               (camera ? camera->GetPos() : vec3(0.0f, 1.0f, 0.0f)));
            mo->is_player = true;
            scenegraph_->GetPlayerCharacterIDs(engine->avatar_ids);
            engine->num_avatars = static_cast<int>(engine->avatar_ids.size());
        }

        if (online->IsActive()) {
            Input::Instance()->SetUpForXPlayers(online->LocalLobbyPlayerCount(), true);
        } else {
            Input::Instance()->SetUpForXPlayers(engine->num_avatars);
        }

        bool focused_character_set = false;
        /*
        for (uint32_t i = 0; i < num_npc_avatars; i++) {

                MovementObject* avatar = static_cast<MovementObject*>(scenegraph_->GetObjectFromID(avatar_ids[i]));
                if (Engine::Instance()->multiplayer.IsActive() && !Engine::Instance()->multiplayer.IsHosting()) {
                        avatar->ChangeControlScript(scenegraph_->level->GetNPCMPScript());
                }
        }
        */

        std::vector<Possession> possessions = GeneratePossessionList();

        for (Possession possession : possessions) {
            MovementObject* avatar = static_cast<MovementObject*>(scenegraph_->GetObjectFromID(possession.avatar));

            if (avatar != nullptr) {
                avatar->ChangeControlScript(scenegraph_->level->GetPCScript(avatar));

                avatar->controlled = true;
                avatar->controller_id = possession.controller_id;
                avatar->remote = !possession.is_local;

                if (possession.is_local) {
                    // Changing the focused value should only occur if we're playing in split-screen, as it's the only
                    // situation where it's relevant to dynamically shift the audio source, and the underlying reason why
                    // we have this concept in the engine in the first place.
                    if (engine->GetSplitScreen()) {
                        // TODO:
                        // Getting the knocked_out value here from the angel script env is pretty rough.
                        // We should reconsider this approach. /Max
                        if (!focused_character_set && avatar->ASGetIntVar("knocked_out") == MovementObject::_awake) {
                            avatar->focused_character = true;
                            focused_character_set = true;
                        } else {
                            avatar->focused_character = false;
                        }
                    } else {
                        if (!avatar->focused_character && !focused_character_set && !avatar->remote && Online::Instance()->IsHosting()) {
                            Online::Instance()->PossessAvatar(Online::Instance()->online_session->local_player_id, avatar->GetID());
                        }

                        // Set the first player controlled avatar as the focused one, this works
                        // in single player because we only expect there to be one controllable
                        // avatar.
                        avatar->focused_character = !focused_character_set;
                        focused_character_set = true;
                    }
                }

                avatar->camera_id = possession.camera_id;
            }
        }
        Camera* editor_camera = ActiveCameras::GetCamera(0);
        if (editor_camera != nullptr && editor_camera->getCameraObject() != nullptr) {
            editor_camera->getCameraObject()->controlled = false;
        }
    }
}
