// Stable, balanced escort assignments with arbitrary friendship eligibility.
// Apache-2.0, like the rest of the engine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <vector>

namespace BotFollowAssignments {
using ID = uint32_t;
constexpr ID kNone = std::numeric_limits<ID>::max();
struct Candidate { ID leader; double distance_squared; };
struct Follower {
    ID bot;
    ID previous = kNone;
    std::vector<Candidate> candidates;
};

// First retain valid relationships. Move only enough followers to balance the
// loads that friendship permits; distance breaks ties instead of attracting
// every bot to the closest person. Alternating paths handle overlapping teams.
inline std::map<ID, ID> Assign(std::vector<Follower> followers) {
    std::sort(followers.begin(), followers.end(), [](const Follower& a, const Follower& b) { return a.bot < b.bot; });
    std::vector<std::map<ID, double>> eligible(followers.size());
    std::map<ID, int> loads;
    std::vector<ID> assigned(followers.size(), kNone);
    for (size_t i = 0; i < followers.size(); ++i) {
        for (const auto& candidate : followers[i].candidates) {
            if (candidate.leader == kNone) continue;
            const double distance = std::isfinite(candidate.distance_squared) ? std::max(0.0, candidate.distance_squared) : 1e30;
            auto inserted = eligible[i].emplace(candidate.leader, distance);
            if (!inserted.second) inserted.first->second = std::min(inserted.first->second, distance);
            loads.emplace(candidate.leader, 0);
        }
        if (eligible[i].count(followers[i].previous)) {
            assigned[i] = followers[i].previous;
            ++loads[assigned[i]];
        }
    }
    for (size_t i = 0; i < followers.size(); ++i) {
        if (assigned[i] != kNone || eligible[i].empty()) continue;
        auto best = eligible[i].begin();
        for (auto it = eligible[i].begin(); it != eligible[i].end(); ++it)
            if (loads[it->first] < loads[best->first] ||
                (loads[it->first] == loads[best->first] && it->second < best->second)) best = it;
        assigned[i] = best->first;
        ++loads[best->first];
    }
    struct Move { size_t follower; ID leader; };
    for (;;) {
        int minimum = std::numeric_limits<int>::max(), maximum = 0;
        for (const auto& load : loads) { minimum = std::min(minimum, load.second); maximum = std::max(maximum, load.second); }
        if (maximum - minimum <= 1 || loads.empty()) break;
        std::vector<Move> best_moves;
        int best_gap = 1, best_churn = std::numeric_limits<int>::max();
        double best_distance = 0;
        for (const auto& start : loads) {
            if (start.second - minimum < 2) continue;
            std::map<ID, std::pair<ID, size_t>> parent;
            std::queue<ID> todo;
            parent[start.first] = {start.first, followers.size()};
            todo.push(start.first);
            while (!todo.empty()) {
                const ID from = todo.front(); todo.pop();
                // Prefer a newly assigned bot before disrupting an established
                // relationship when both paths rebalance the same workload.
                std::vector<size_t> residents;
                for (size_t i = 0; i < assigned.size(); ++i) if (assigned[i] == from) residents.push_back(i);
                std::stable_sort(residents.begin(), residents.end(), [&](size_t a, size_t b) {
                    return (assigned[a] == followers[a].previous) < (assigned[b] == followers[b].previous);
                });
                for (size_t i : residents) for (const auto& candidate : eligible[i]) {
                    if (parent.count(candidate.first)) continue;
                    parent[candidate.first] = {from, i};
                    todo.push(candidate.first);
                }
            }
            for (const auto& reached : parent) {
                const int gap = start.second - loads[reached.first];
                if (gap < 2) continue;
                std::vector<Move> moves;
                for (ID end = reached.first; end != start.first; end = parent[end].first)
                    moves.push_back({parent[end].second, end});
                int churn = 0;
                double distance = 0;
                for (const auto& move : moves) {
                    const size_t i = move.follower;
                    churn += int(move.leader != followers[i].previous) - int(assigned[i] != followers[i].previous);
                    distance += eligible[i].at(move.leader) - eligible[i].at(assigned[i]);
                }
                if (gap > best_gap || (gap == best_gap && (best_moves.empty() || moves.size() < best_moves.size() ||
                    (moves.size() == best_moves.size() && (churn < best_churn || (churn == best_churn && distance < best_distance)))))) {
                    best_gap = gap; best_moves = moves; best_churn = churn; best_distance = distance;
                }
            }
        }
        if (best_moves.empty()) break;
        for (const auto& move : best_moves) {
            --loads[assigned[move.follower]];
            assigned[move.follower] = move.leader;
            ++loads[move.leader];
        }
    }
    std::map<ID, ID> result;
    for (size_t i = 0; i < followers.size(); ++i)
        if (assigned[i] != kNone) result[followers[i].bot] = assigned[i];
    return result;
}
}
