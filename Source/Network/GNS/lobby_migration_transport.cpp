// Reachability checks for a deliberate, pregame server handoff. Apache-2.0.
#include "gns_net_framework.h"

#ifdef USE_GNS_NETWORK_FRAMEWORK
#include <cstring>
#include <vector>

namespace {
const char* kProbePrefix = "OGMP-HANDOFF-5:";
}

bool GNSNetFramework::RemoteAddress(NetConnectionID id, std::string* ip, uint64_t* steam_id) const {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    SteamNetConnectionInfo_t info;
    if (!isns || !isns->GetConnectionInfo(Handle(id), &info)) return false;
    char address[SteamNetworkingIPAddr::k_cchMaxString] = {};
    info.m_addrRemote.ToString(address, sizeof(address), true);
    *ip = address;
    *steam_id = info.m_identityRemote.GetSteamID64();
    return !ip->empty() || *steam_id != 0;
}

bool GNSNetFramework::BeginMigrationListener(const std::string& secret) {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    StopMigrationProbes();
    if (!isns || secret.empty()) return false;
    migration_secret = secret;
    migration_reply = "ready";
    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = DefaultPort();
    migration_listener = isns->CreateListenSocketIP(address, 0, nullptr);
#ifdef OVERHAUL_STEAM_SOCKETS
    migration_p2p_listener = isns->CreateListenSocketP2P(0, 0, nullptr);
#endif
    if (migration_listener == k_HSteamListenSocket_Invalid && migration_p2p_listener == k_HSteamListenSocket_Invalid) {
        migration_error = "The new host cannot open its multiplayer port.";
        return false;
    }
    return true;
}

bool GNSNetFramework::BeginMigrationProbe(const std::string& ip, uint64_t steam_id, const std::string& secret) {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    StopMigrationProbes();
    if (!isns || secret.empty()) return false;
    migration_secret = secret;
#ifdef OVERHAUL_STEAM_SOCKETS
    if (steam_id != 0) {
        SteamNetworkingIdentity identity;
        identity.Clear();
        identity.SetSteamID64(steam_id);
        migration_probe = isns->ConnectP2P(identity, 0, 0, nullptr);
    } else
#else
    (void)steam_id;
#endif
    {
        SteamNetworkingIPAddr address;
        address.Clear();
        if (!address.ParseString(ip.c_str())) return false;
        if (!address.m_port) address.m_port = DefaultPort();
        migration_probe = isns->ConnectByIPAddress(address, 0, nullptr);
    }
    return migration_probe != k_HSteamNetConnection_Invalid;
}

bool GNSNetFramework::HandleMigrationStatus(SteamNetConnectionStatusChangedCallback_t* event) {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    const bool incoming = migration_incoming.count(event->m_hConn) != 0 ||
        (migration_listener != k_HSteamListenSocket_Invalid && event->m_info.m_hListenSocket == migration_listener) ||
        (migration_p2p_listener != k_HSteamListenSocket_Invalid && event->m_info.m_hListenSocket == migration_p2p_listener);
    if (!incoming && event->m_hConn != migration_probe) return false;
    if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && incoming) {
        migration_incoming[event->m_hConn] = false;
        isns->AcceptConnection(event->m_hConn);
    } else if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected && !incoming) {
        const std::string hello = std::string(kProbePrefix) + migration_secret;
        isns->SendMessageToConnection(event->m_hConn, hello.data(), static_cast<uint32_t>(hello.size()), k_nSteamNetworkingSend_Reliable, nullptr);
    } else if (event->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
               event->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        if (!incoming) migration_error = "The new host is not reachable. Check its multiplayer port or Steam connection.";
        migration_incoming.erase(event->m_hConn);
        isns->CloseConnection(event->m_hConn, 0, "handoff probe closed", false);
    }
    return true;
}

void GNSNetFramework::UpdateMigrationProbes() {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    if (!isns || migration_secret.empty()) return;
    const std::string prefix = std::string(kProbePrefix) + migration_secret;
    const auto receive = [&](HSteamNetConnection connection, bool incoming) {
        for (unsigned n = 0; n < 8; ++n) {
            SteamNetworkingMessage_t* message = nullptr;
            if (isns->ReceiveMessagesOnConnection(connection, &message, 1) <= 0) break;
            if (message->m_cbSize > 0 && message->m_cbSize <= 4096) {
                const std::string text(static_cast<const char*>(message->m_pData), message->m_cbSize);
                if (incoming && text == prefix) migration_incoming[connection] = true;
                else if (!incoming && text.compare(0, prefix.size() + 1, prefix + "|") == 0)
                    migration_received = text.substr(prefix.size() + 1);
            }
            message->Release();
        }
    };
    for (const auto& entry : migration_incoming) receive(entry.first, true);
    if (migration_probe != k_HSteamNetConnection_Invalid) receive(migration_probe, false);
    const int64_t now = utils->GetLocalTimestamp();
    if (now - migration_last_reply < 100000) return;
    migration_last_reply = now;
    const std::string reply = prefix + "|" + migration_reply;
    for (const auto& entry : migration_incoming) if (entry.second)
        isns->SendMessageToConnection(entry.first, reply.data(), static_cast<uint32_t>(reply.size()), k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
}

void GNSNetFramework::SetMigrationReply(const std::string& reply) {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    migration_reply = reply;
}

std::string GNSNetFramework::MigrationReply() const {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    return migration_received;
}

std::string GNSNetFramework::MigrationError() const {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    return migration_error;
}

void GNSNetFramework::StopMigrationProbes() {
    std::lock_guard<std::recursive_mutex> guard(maps_lock);
    if (isns) {
        for (const auto& entry : migration_incoming) isns->CloseConnection(entry.first, 0, "handoff finished", false);
        if (migration_probe != k_HSteamNetConnection_Invalid) isns->CloseConnection(migration_probe, 0, "handoff finished", false);
        if (migration_listener != k_HSteamListenSocket_Invalid) isns->CloseListenSocket(migration_listener);
        if (migration_p2p_listener != k_HSteamListenSocket_Invalid) isns->CloseListenSocket(migration_p2p_listener);
    }
    migration_incoming.clear();
    migration_probe = k_HSteamNetConnection_Invalid;
    migration_listener = migration_p2p_listener = k_HSteamListenSocket_Invalid;
    migration_secret.clear(); migration_reply.clear(); migration_received.clear(); migration_error.clear();
    migration_last_reply = 0;
}
#endif
