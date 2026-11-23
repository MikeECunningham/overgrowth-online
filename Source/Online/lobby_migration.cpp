// Deliberate server handoff before a match. The old lobby stays alive during
// reachability checks; no loaded world or live simulation is migrated.
// Copyright 2026. Licensed under the Apache License, Version 2.0.
#include <Online/online.h>
#include <Online/Message/lobby_control.h>
#include <Internal/config.h>
#include <JSON/json.h>
#include <Logging/logdata.h>

#include <chrono>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace {
enum class Phase {
    Idle, CoordinatorInit, AwaitListener, Checking, CandidateReady, Probe,
    CandidateCommit, ClientCommit, OldHostWaiting, CandidateHosting,
    ClientJoining, RollbackHosting, RollbackJoining, Complete
};
struct Handoff {
    Phase phase = Phase::Idle;
    std::string status, secret, snapshot, join_token, ip, rollback_ip;
    uint64_t steam = 0, rollback_steam = 0;
    PeerID origin = 255, candidate = 0;
    PlayerID target = 0, actor = 0, original_host = 255;
    std::map<PeerID, std::string> tokens;
    std::set<PeerID> ready, admitted;
    double deadline = 0, due = 0;
    bool restarting = false;
};
Handoff handoff;
double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::string Encode(const Json::Value& value) { return Json::FastWriter().write(value); }
std::string Secret() {
    std::random_device random;
    std::ostringstream result;
    for (unsigned i = 0; i < 8; ++i) result << std::hex << std::setw(8) << std::setfill('0') << random();
    return result.str();
}
bool Coordinating() {
    return handoff.phase == Phase::AwaitListener || handoff.phase == Phase::Checking ||
           handoff.phase == Phase::OldHostWaiting;
}
Json::Value Message(const char* action) {
    Json::Value value;
    value["action"] = action;
    value["secret"] = handoff.secret;
    return value;
}
void Done(const std::string& status) {
    handoff.phase = Phase::Idle;
    handoff.status = status;
    handoff.join_token.clear();
    handoff.tokens.clear();
    handoff.ready.clear();
    handoff.admitted.clear();
}
}

std::string Online::MigrationJoinToken() const { return handoff.join_token; }
unsigned Online::MigrationOriginPeer() const { return handoff.origin; }
void Online::ConfigureClientMigrationHandshake(const std::string& token, PeerID origin) {
    handoff.join_token = token;
    handoff.origin = origin;
}
std::string Online::LobbyMigrationStatus() const { return handoff.status; }
bool Online::LobbyMigrationBusy() const { return handoff.phase != Phase::Idle && handoff.phase != Phase::Complete; }

void Online::OnLobbySessionStopping() {
    const bool transport_lost = lobby_migration_transport_lost_.exchange(false);
    if (handoff.restarting) return;
    if (transport_lost && handoff.phase == Phase::ClientJoining) {
        // Engine shutdown runs before LateUpdate. Keep the credentials and
        // original snapshot so the main-thread handoff can roll back next.
        handoff.deadline = Now();
        handoff.status = "The new host disconnected; restoring the original lobby...";
        return;
    }
    if (transport_lost && handoff.phase == Phase::RollbackJoining) {
        // The original server may still be switching roles. Retry within the
        // existing rollback deadline instead of losing the admission token.
        handoff.due = Now() + 0.5;
        return;
    }
#ifdef USE_GNS_NETWORK_FRAMEWORK
    net.StopMigrationProbes();
#endif
    Done("");
}

bool Online::BeginLobbyHostMigration(PlayerID target, PlayerID actor) {
    if (!IsHosting() || !LobbyWaiting() || LobbyMigrationBusy() || !CanPromoteLobbyHost(target, actor)) return false;
#ifdef USE_GNS_NETWORK_FRAMEWORK
    handoff = Handoff();
    handoff.target = target;
    handoff.actor = actor;
    handoff.phase = Phase::CoordinatorInit;
    handoff.status = "Preparing host transfer...";
    return true;
#else
    handoff.status = "Host transfer needs the GameNetworkingSockets transport.";
    return false;
#endif
}

bool Online::AdmitMigratingPeer(PeerID new_peer, const std::string& token, PeerID old_peer) {
    if (!IsHosting() || (handoff.phase != Phase::CandidateHosting && handoff.phase != Phase::RollbackHosting)) return false;
    const auto expected = handoff.tokens.find(old_peer);
    if (token.empty() || expected == handoff.tokens.end() || expected->second != token || handoff.admitted.count(old_peer)) return false;
    if (!RestoreMigratedPeer(new_peer, old_peer)) return false;
    handoff.admitted.insert(old_peer);
    return true;
}

bool Online::HandleLobbyMigrationControl(PeerID from, const std::string& payload) {
    Json::Value message;
    Json::Reader reader;
    if (!reader.parse(payload, message, false)) return false;
    const std::string action = message.get("action", "").asString();
    if (action.compare(0, 8, "handoff_") != 0) return false;
#ifdef USE_GNS_NETWORK_FRAMEWORK
    const std::string secret = message.get("secret", "").asString();
    if (action == "handoff_prepare") {
        if (!IsClient() || from != 0 || !LobbyWaiting() || LobbyMigrationBusy() || secret.size() < 32 ||
            !message["snapshot"].isObject() || !message["join_token"].isString()) return true;
        handoff = Handoff();
        handoff.secret = secret;
        handoff.snapshot = Encode(message["snapshot"]);
        handoff.origin = LocalPeerID();
        handoff.target = message.get("target", 0).asUInt();
        handoff.candidate = static_cast<PeerID>(message.get("candidate", 0).asUInt());
        handoff.original_host = message["snapshot"].get("decision_host", 255).asUInt();
        handoff.join_token = message["join_token"].asString();
        handoff.ip = message.get("ip", "").asString();
        handoff.steam = message.get("steam", Json::UInt64(0)).asUInt64();
        handoff.deadline = Now() + 20;
        if (Peer* host = GetPeerFromID(0)) net.RemoteAddress(host->conn_id, &handoff.rollback_ip, &handoff.rollback_steam);
        if (handoff.origin == handoff.candidate) {
            handoff.deadline = Now() + 45;
            for (const auto& row : message["tokens"])
                handoff.tokens[static_cast<PeerID>(row["peer"].asUInt())] = row["token"].asString();
            handoff.phase = Phase::CandidateReady;
            handoff.status = "Opening the new host's multiplayer port...";
            Json::Value response = Message("handoff_listener");
            response["ok"] = net.BeginMigrationListener(secret);
            response["port"] = static_cast<unsigned>(NetFramework::DefaultPort());
            response["token"] = handoff.join_token;
            SubmitLobbyRequest(Encode(response));
        } else {
            handoff.phase = Phase::Probe;
            handoff.status = "Checking connection to the new host...";
            net.BeginMigrationProbe(handoff.ip, handoff.steam, secret);
        }
        return true;
    }
    if (secret.empty() || secret != handoff.secret) return true;
    if (action == "handoff_cancel") {
        if (IsClient() && from == 0) {
            net.StopMigrationProbes();
            Done(message.get("reason", "Host transfer cancelled.").asString());
        }
        return true;
    }
    if (action == "handoff_finish") {
        if (IsClient() && from == 0 && handoff.phase == Phase::ClientJoining) {
            handoff.phase = Phase::Complete;
            handoff.status = "Host transferred.";
            handoff.due = Now() + 1;
        }
        return true;
    }
    if (action == "handoff_commit") {
        if (IsClient() && from == 0 && (handoff.phase == Phase::CandidateReady || handoff.phase == Phase::Probe)) {
            handoff.phase = handoff.origin == handoff.candidate ? Phase::CandidateCommit : Phase::ClientCommit;
            handoff.due = Now() + (handoff.phase == Phase::ClientCommit ? 0.6 : 0);
            handoff.deadline = Now() + 25;
            handoff.status = "Moving the lobby to its new host...";
        }
        return true;
    }
    if (!IsHosting() || !Coordinating()) return true;
    const auto expected = handoff.tokens.find(from);
    if (expected == handoff.tokens.end() || message.get("token", "").asString() != expected->second) return true;
    if (action == "handoff_listener" && from == handoff.candidate && handoff.phase == Phase::AwaitListener) {
        if (!message.get("ok", false).asBool()) {
            Json::Value cancel = Message("handoff_cancel");
            cancel["reason"] = "The new host cannot open its multiplayer port; the original lobby is unchanged.";
            Send<OnlineMessages::LobbyControl>(Encode(cancel));
            net.StopMigrationProbes(); Done(cancel["reason"].asString());
            return true;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        if (!address.ParseString(handoff.ip.c_str())) handoff.ip.clear();
        else {
            address.m_port = static_cast<uint16_t>(message.get("port", static_cast<unsigned>(NetFramework::DefaultPort())).asUInt());
            char text[SteamNetworkingIPAddr::k_cchMaxString];
            address.ToString(text, sizeof(text), true);
            handoff.ip = text;
        }
        handoff.phase = Phase::Checking;
        handoff.ready.insert(handoff.candidate);
        handoff.status = "Checking that every client can reach the new host...";
        handoff.deadline = Now() + 20;
        for (const auto& peer : online_session->connected_peers) if (peer.peer_id != handoff.candidate) {
            Json::Value prepare = Message("handoff_prepare");
            Json::Value snapshot; reader.parse(handoff.snapshot, snapshot, false);
            prepare["snapshot"] = snapshot; prepare["target"] = handoff.target; prepare["candidate"] = handoff.candidate;
            prepare["join_token"] = handoff.tokens.at(peer.peer_id);
            prepare["ip"] = handoff.ip; prepare["steam"] = Json::UInt64(handoff.steam);
            SendTo<OnlineMessages::LobbyControl>(peer.conn_id, Encode(prepare));
        }
        net.BeginMigrationProbe(handoff.ip, handoff.steam, handoff.secret);
    } else if (action == "handoff_ready" && handoff.phase == Phase::Checking) {
        handoff.ready.insert(from);
    }
#endif
    return true;
}

void Online::UpdateLobbyHostMigration() {
#ifdef USE_GNS_NETWORK_FRAMEWORK
    if (handoff.phase == Phase::Idle) return;
    const double now = Now();
    if (handoff.phase == Phase::Complete) {
        if (now >= handoff.due) { net.StopMigrationProbes(); Done(handoff.status); }
        return;
    }
    if (handoff.phase == Phase::CoordinatorInit) {
        if (!IsHosting() || !CanPromoteLobbyHost(handoff.target, handoff.actor)) { Done("Host transfer cancelled."); return; }
        const auto target = online_session->player_states.find(handoff.target);
        if (target == online_session->player_states.end()) { Done("That player left the lobby."); return; }
        handoff.candidate = target->second.peer_id;
        const auto peers = GetPeers();
        Peer destination;
        bool found = false;
        for (const auto& peer : peers) if (peer.peer_id == handoff.candidate) { destination = peer; found = true; }
        if (!found || !net.RemoteAddress(destination.conn_id, &handoff.ip, &handoff.steam)) { Done("Cannot find the new host's address."); return; }
        handoff.secret = Secret();
        handoff.snapshot = ExportLobbyForMigration();
        Json::Reader reader; Json::Value snapshot; reader.parse(handoff.snapshot, snapshot, false);
        handoff.original_host = snapshot.get("decision_host", 255).asUInt();
        handoff.tokens[255] = Secret();
        for (const auto& peer : peers) handoff.tokens[peer.peer_id] = Secret();
        handoff.join_token = handoff.tokens.at(255);
        handoff.origin = 255;
        Json::Value prepare = Message("handoff_prepare");
        prepare["snapshot"] = snapshot; prepare["target"] = handoff.target; prepare["candidate"] = handoff.candidate;
        prepare["join_token"] = handoff.tokens.at(handoff.candidate);
        prepare["tokens"] = Json::Value(Json::arrayValue);
        for (const auto& token : handoff.tokens) {
            Json::Value row; row["peer"] = token.first; row["token"] = token.second; prepare["tokens"].append(row);
        }
        handoff.phase = Phase::AwaitListener;
        handoff.deadline = now + 20;
        SendTo<OnlineMessages::LobbyControl>(destination.conn_id, Encode(prepare));
        return;
    }
    if ((handoff.phase == Phase::Checking || handoff.phase == Phase::Probe) && net.MigrationReply() == "ready") {
        if (handoff.phase == Phase::Checking) handoff.ready.insert(255);
        else if (handoff.ready.empty()) {
            Json::Value response = Message("handoff_ready"); response["token"] = handoff.join_token;
            SubmitLobbyRequest(Encode(response)); handoff.ready.insert(handoff.origin);
        }
    }
    if (handoff.phase == Phase::Checking && handoff.ready.size() == handoff.tokens.size()) {
        Send<OnlineMessages::LobbyControl>(Encode(Message("handoff_commit")));
        handoff.phase = Phase::OldHostWaiting;
        handoff.deadline = now + 25;
        handoff.status = "Waiting for the other clients to join the new host...";
        return;
    }
    if (handoff.phase == Phase::CandidateCommit && now >= handoff.due) {
        handoff.restarting = true;
        StopMultiplayer();
        StartHostingMultiplayer();
        const bool restored = RestoreLobbyForMigration(handoff.snapshot, handoff.origin, handoff.target);
        handoff.restarting = false;
        if (!restored) { net.StopMigrationProbes(); Done("The lobby could not be restored on the new host."); return; }
        OpenToFriends(false);
        handoff.admitted.insert(handoff.origin);
        handoff.phase = Phase::CandidateHosting;
        handoff.status = "New host ready; restoring the remaining clients...";
        net.SetMigrationReply("hosting");
        return;
    }
    if (handoff.phase == Phase::CandidateHosting) {
        bool others_ready = true;
        for (const auto& token : handoff.tokens) if (token.first != 255 && !handoff.admitted.count(token.first)) others_ready = false;
        if (others_ready) net.SetMigrationReply("joined");
        if (handoff.admitted.size() == handoff.tokens.size()) {
            Send<OnlineMessages::LobbyControl>(Encode(Message("handoff_finish")));
            handoff.phase = Phase::Complete; handoff.due = now + 1;
            handoff.status = "Host transferred. The original host may disconnect.";
            net.SetMigrationReply("finished");
            return;
        }
    }
    if (handoff.phase == Phase::OldHostWaiting && (net.MigrationReply() == "joined" || net.MigrationReply() == "finished")) {
        handoff.phase = Phase::ClientCommit; handoff.due = now;
    }
    if (handoff.phase == Phase::ClientCommit && now >= handoff.due) {
        handoff.restarting = true;
        StopMultiplayer();
        if (handoff.steam) ConnectP2P(handoff.steam); else ConnectByIPAddress(handoff.ip);
        handoff.restarting = false;
        handoff.phase = Phase::ClientJoining;
        handoff.status = "Rejoining the lobby on its new host...";
        return;
    }
    if (handoff.phase == Phase::RollbackJoining) {
        if (IsClient() && LobbyWaiting() && LobbyHostPlayerID() == handoff.original_host) {
            Json::Value snapshot; Json::Reader reader;
            reader.parse(handoff.snapshot, snapshot, false);
            std::set<PlayerID> expected, present;
            for (const auto& row : snapshot["players"])
                if (row.get("peer", 0).asUInt() == handoff.origin) expected.insert(row.get("id", 0).asUInt());
            for (const auto& state : GetLocalPlayerStates()) present.insert(state.player_id);
            if (!expected.empty() && present == expected) {
                net.StopMigrationProbes();
                Done("Reconnected to the original lobby.");
                return;
            }
        }
        if (now < handoff.deadline && !IsActive() && !IsAwaitingShutdown() && now >= handoff.due) {
            handoff.restarting = true;
            if (handoff.rollback_steam) ConnectP2P(handoff.rollback_steam); else ConnectByIPAddress(handoff.rollback_ip);
            handoff.restarting = false;
            handoff.due = now + 0.5;
        }
    }
    if (handoff.phase == Phase::RollbackHosting) {
        bool survivors_restored = true;
        for (const auto& token : handoff.tokens)
            if (token.first != 255 && token.first != handoff.candidate && !handoff.admitted.count(token.first))
                survivors_restored = false;
        if (survivors_restored) {
            // The failed candidate must not hold everyone else in a frozen
            // lobby for the full reconnect timeout. It can join normally if
            // it comes back after this rollback has completed.
            net.StopMigrationProbes();
            Done("Original lobby restored. The failed host can reconnect.");
            return;
        }
    }
    if (now < handoff.deadline) return;
    if (handoff.phase == Phase::AwaitListener || handoff.phase == Phase::Checking) {
        Json::Value cancel = Message("handoff_cancel");
        cancel["reason"] = "Host transfer cancelled: a client could not reach the new host. Check its UDP port or Steam connection.";
        if (IsHosting()) Send<OnlineMessages::LobbyControl>(Encode(cancel));
        net.StopMigrationProbes(); Done(cancel["reason"].asString());
    } else if (handoff.phase == Phase::Probe || handoff.phase == Phase::CandidateReady) {
        net.StopMigrationProbes(); Done("Host transfer cancelled; staying in the original lobby.");
    } else if (handoff.phase == Phase::OldHostWaiting && IsHosting()) {
        // The original server remains reachable until everyone else has moved.
        RestoreLobbyForMigration(handoff.snapshot, 255, handoff.original_host);
        handoff.admitted.clear(); handoff.admitted.insert(255);
        handoff.phase = Phase::RollbackHosting;
        handoff.deadline = now + 45;
        handoff.status = "Transfer did not complete; restoring the original lobby...";
        net.StopMigrationProbes();
    } else if (handoff.phase == Phase::RollbackHosting) {
        net.StopMigrationProbes(); Done("The original host is hosting again.");
    } else if (handoff.phase == Phase::CandidateHosting || handoff.phase == Phase::ClientJoining) {
        handoff.restarting = true;
        StopMultiplayer();
        net.StopMigrationProbes();
        if (handoff.origin == 255) {
            // The coordinator has already stopped its listener to join the
            // candidate. It must become the original server again, not try
            // connecting to its own unset rollback address.
            StartHostingMultiplayer();
            const bool restored = RestoreLobbyForMigration(handoff.snapshot, 255, handoff.original_host);
            handoff.restarting = false;
            if (!restored) { Done("The original lobby could not be restored."); return; }
            handoff.admitted.clear(); handoff.admitted.insert(255);
            handoff.phase = Phase::RollbackHosting;
            handoff.deadline = now + 45;
            handoff.status = "The original host is back; restoring the remaining clients...";
            return;
        }
        if (handoff.rollback_steam) ConnectP2P(handoff.rollback_steam); else ConnectByIPAddress(handoff.rollback_ip);
        handoff.restarting = false;
        handoff.phase = Phase::RollbackJoining; handoff.due = now + 0.5; handoff.deadline = now + 45;
        handoff.status = "Transfer did not complete; reconnecting to the original host...";
    } else if (handoff.phase == Phase::RollbackJoining) {
        handoff.restarting = true;
        StopMultiplayer();
        handoff.restarting = false;
        net.StopMigrationProbes();
        Done("Could not reconnect to the original host.");
    } else { net.StopMigrationProbes(); Done("Host transfer cancelled."); }
#endif
}
