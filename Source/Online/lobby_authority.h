#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

// Decision roles follow logical players, independently of transport ownership
// and the order in which local input devices were connected.
namespace LobbyAuthority {
constexpr unsigned kMaxSlots = 4096;
using Player = uint32_t;
using Peer = uint8_t;
constexpr Player InvalidPlayer = std::numeric_limits<Player>::max();
struct Seat { Player id; Peer peer; unsigned local_index; };

inline Player AllocatePlayerID(const std::vector<Seat>& seats, Peer peer, unsigned local_index, Player& next) {
    const auto used = [&seats](Player id) {
        for (const auto& seat : seats) if (seat.id == id) return true;
        return false;
    };
    // A migrated logical player may already own the number assigned to a new
    // transport peer. Admission must never overwrite that surviving player.
    if (local_index == 0 && !used(peer)) return peer;
    while (used(next)) ++next;
    return next++;
}

struct State {
    Player host = 255;
    std::map<Peer, Player> leaders;

    const Seat* Find(const std::vector<Seat>& seats, Player id) const {
        for (const auto& seat : seats) if (seat.id == id) return &seat;
        return nullptr;
    }
    bool IsLeader(const std::vector<Seat>& seats, Player id) const {
        const Seat* seat = Find(seats, id);
        if (!seat) return false;
        const auto leader = leaders.find(seat->peer);
        return leader != leaders.end() && leader->second == id;
    }
    void Refresh(std::vector<Seat> seats) {
        std::sort(seats.begin(), seats.end(), [](const Seat& a, const Seat& b) {
            if (a.peer != b.peer) return a.peer == 255 || (b.peer != 255 && a.peer < b.peer);
            return a.local_index < b.local_index;
        });
        for (auto it = leaders.begin(); it != leaders.end();) {
            const Seat* seat = Find(seats, it->second);
            if (!seat || seat->peer != it->first) it = leaders.erase(it); else ++it;
        }
        for (const auto& seat : seats) if (!leaders.count(seat.peer)) leaders[seat.peer] = seat.id;
        if (!Find(seats, host)) host = seats.empty() ? InvalidPlayer : leaders.at(seats.front().peer);
    }
    bool CanPromoteHost(const std::vector<Seat>& seats, Player actor, Player target) const {
        return actor == host && actor != target && Find(seats, actor) && Find(seats, target);
    }
    bool CanPromoteLeader(const std::vector<Seat>& seats, Player actor, Player target) const {
        const Seat* a = Find(seats, actor); const Seat* t = Find(seats, target);
        return a && t && a->peer == t->peer && actor != target && IsLeader(seats, actor);
    }
    bool CanRemove(const std::vector<Seat>& seats, Player actor, Player target) const {
        const Seat* a = Find(seats, actor); const Seat* t = Find(seats, target);
        if (!a || !t || target == host) return false;
        if (a->peer != t->peer) return actor == host;
        return actor != target && IsLeader(seats, actor);
    }
    bool CanSetTeam(const std::vector<Seat>& seats, Player actor, Player target,
                    bool host_controls, bool mode_uses_lobby_teams) const {
        return Find(seats, actor) && Find(seats, target) &&
            (actor == host || (!host_controls && mode_uses_lobby_teams && actor == target));
    }
};
}  // namespace LobbyAuthority
