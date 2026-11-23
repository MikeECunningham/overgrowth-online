//-----------------------------------------------------------------------------
//           Name: online_connection_states.cpp
//      Developer: Wolfire Games LLC
//    Modified By: Overgrowth Online (crxhvrd)
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
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
#include "online_connection_states.h"

#include <Online/online.h>
#include <Online/online_utility.h>

#include <Main/engine.h>
#include <Version/version.h>

// ---------------- StateStateSuspended ----------------

BaseState<PendingConnection>* StateSuspended::OnUpdate() {
    return this;
}

// ---------------- StateVersionCheck ----------------

void StateVersionCheck::OnEnter() {
    Online::Instance()->SendTo<OnlineMessages::PCSBuildVersionRequestMessage>(actor->net_connection_id);
}

BaseState<PendingConnection>* StateVersionCheck::OnUpdate() {
    if (actor->received_version) {
        // Release + wire protocol decide compatibility. A CI build number is
        // diagnostic metadata and cannot override an incompatible identity.
        const std::string& ours = GetNetworkVersion();
        if (IsNetworkVersionCompatible(actor->oo_version)) {
            if (actor->build_id != GetBuildID()) {
                LOGI << "Matching Overgrowth Online " << ours << " with different build IDs: client "
                     << actor->build_id << ", host " << GetBuildID() << std::endl;
            }
            return new StateHandshake();
        }
        const std::string theirs = actor->oo_version.empty() ? "an older version" : actor->oo_version;
        Online::Instance()->AddLocalChatMessage("A player with Overgrowth Online " + theirs + " tried to join; you have " +
                                               ours + ". Everyone needs the same version and network protocol.");
        LOGI << "Overgrowth Online version mismatch: client " << theirs << ", host " << ours << std::endl;
        const bool client_older = OnlineUtility::CompareVersions(actor->oo_version, ours) < 0;
        Online::Instance()->CloseConnection(actor->net_connection_id, client_older ? ConnectionClosedReason::CLIENT_OUTDATED
                                                                                 : ConnectionClosedReason::SERVER_OUTDATED);
        return nullptr;
    }
    return this;
}

// ---------------- StateHandshake ----------------

void StateHandshake::OnEnter() {
    // Send session parameters
    Online::Instance()->SendTo<OnlineMessages::PCSSessionParametersMessage>(actor->net_connection_id, Online::Instance()->online_session->binding_id_map_);
}

BaseState<PendingConnection>* StateHandshake::OnUpdate() {
    if (actor->received_client_params) {
        if (!OnlineUtility::IsValidPlayerName(actor->player_name)) {
            actor->player_name = "InvalidName";
        }

        if (actor->active_mods_string != OnlineUtility::GetActiveModsString()) {
            Online::Instance()->AddLocalChatMessage("\"" + actor->player_name + "\" tried to connect, but was rejected entry due to a mod mismatch");
            Online::Instance()->AddLocalChatMessage(" - Expected mods: \"" + OnlineUtility::GetActiveModsString() + "\"");
            Online::Instance()->AddLocalChatMessage(" - Client's mods: \"" + actor->active_mods_string + "\"");

            Online::Instance()->CloseConnection(actor->net_connection_id, ConnectionClosedReason::MOD_MISMATCH);
            return new StateSuspended();
        }

        Peer* peer = Online::Instance()->GetPeerFromConnection(actor->net_connection_id);
        if (peer == nullptr) return nullptr;
        actor->player_id = peer->peer_id;
        Online::Instance()->SendTo<OnlineMessages::PCSAssignPlayerID>(actor->net_connection_id, actor->player_id);
        if (!actor->migration_token.empty()) {
            if (!Online::Instance()->AdmitMigratingPeer(peer->peer_id, actor->migration_token, actor->migration_origin_peer)) {
                Online::Instance()->CloseConnection(actor->net_connection_id, ConnectionClosedReason::DISCONNECTED);
                return new StateSuspended();
            }
        } else {
            // The captured migration roster reserves logical identities until
            // every expected machine has rejoined. Ordinary fresh admission
            // resumes only after that transfer has completed.
            if (Online::Instance()->LobbyMigrationBusy()) {
                Online::Instance()->CloseConnection(actor->net_connection_id, ConnectionClosedReason::DISCONNECTED);
                return new StateSuspended();
            }
            Online::Instance()->EstablishPeerPlayers(peer->peer_id, actor->local_names.empty() ? std::vector<std::string>{actor->player_name} : actor->local_names, actor->local_screens);
        }
        return Online::Instance()->LobbyWaiting() ? static_cast<BaseState<PendingConnection>*>(new StateLobbyWaiting()) : new StateSync();
    }
    return this;
}

BaseState<PendingConnection>* StateLobbyWaiting::OnUpdate() {
    return Online::Instance()->LobbyWaiting() || Online::Instance()->level_name.empty() ? this : static_cast<BaseState<PendingConnection>*>(new StateSync());
}

// ---------------- StateSync ----------------

void StateSync::OnEnter() {
    // TODO This package is a placeholder, we don't actually send any level files here right now, as this would require a bigger refactor to file transfer
    // For now, we simply tell the client what map to load and *pray* the client loads the exact same map as the host! (pretty much like it did before)
    // Overgrowth Online: a level of the host's own (its write folder) goes as a file first
    Online::Instance()->SendLevelFile(actor->net_connection_id, Online::Instance()->level_name);
    Online::Instance()->SendTo<OnlineMessages::PCSFileTransferMetadataMessage>(actor->net_connection_id, Online::Instance()->level_name, Online::Instance()->campaign_id);

    // TODO initiate level file transfers to client
}

BaseState<PendingConnection>* StateSync::OnUpdate() {
    if (actor->finished_loading)
        return new StateWaiting();
    return this;
}

// ---------------- StateWaiting ----------------

void StateWaiting::OnEnter() {
}

BaseState<PendingConnection>* StateWaiting::OnUpdate() {
    if (Online::Instance()->ForceMapStartOnLoad())
        return new StateEstablished();
    return this;
}

// ---------------- StateEstablished ----------------

void StateEstablished::OnEnter() {
    Online* online = Online::Instance();
    Peer* peer = online->GetPeerFromConnection(actor->net_connection_id);
    if (peer == nullptr) return;
    actor->player_id = peer->peer_id;
    online->AssignPeerAvatars(peer->peer_id);
    online->Send<OnlineMessages::PCSLoadingCompletedMessage>();
    online->BroadcastLobby();

    // Display Join Message
    Online::Instance()->SendRawChatMessage(actor->player_name + " just joined!");

    // Dump playerstates onto new player
    for (const auto& it : Online::Instance()->online_session->player_states) {
        Online::Instance()->SendTo<OnlineMessages::SetPlayerState>(actor->net_connection_id, it.first, &it.second);
    }

    // This seems to be an important package
    Online::Instance()->GenerateStateForNewConnection(actor->net_connection_id);
}

BaseState<PendingConnection>* StateEstablished::OnUpdate() {
    return nullptr;
}
