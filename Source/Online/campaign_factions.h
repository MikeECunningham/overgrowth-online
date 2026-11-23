// Campaign faction profiles preserve the engine's overlapping team tags.
// Apache-2.0, like the rest of the engine.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace CampaignFactions {
struct MainAnchor {
    uint32_t player = std::numeric_limits<uint32_t>::max();
    int team = 0;

    int Resolve(const std::map<uint32_t, int>& humans, uint32_t story_player) const {
        if (team > 0) {
            const auto original = humans.find(player);
            return original != humans.end() ? original->second : team;
        }
        const auto story = humans.find(story_player);
        return story != humans.end() ? story->second : 1;
    }
    void Refresh(const std::map<uint32_t, int>& humans, uint32_t story_player) {
        const auto original = humans.find(player);
        if (team > 0 && original != humans.end()) {
            team = original->second;
            return;
        }
        const auto story = humans.find(story_player);
        // A surviving teammate may inherit the authored actor; an opponent
        // selected by a generic host/leader fallback cannot change factions.
        if (story != humans.end() && (team <= 0 || story->second == team)) {
            player = story_player;
            team = story->second;
        }
    }
};
inline std::vector<std::string> Tags(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream input(text);
    std::string tag;
    while (std::getline(input, tag, ',')) {
        const auto begin = tag.find_first_not_of(' ');
        if (begin == std::string::npos) continue;
        tag = tag.substr(begin, tag.find_last_not_of(' ') - begin + 1);
        result.push_back(tag);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
inline std::string Canonical(const std::string& text) {
    std::string result;
    for (const auto& tag : Tags(text)) {
        if (!result.empty()) result += ',';
        result += tag;
    }
    return result;
}
inline bool Friendly(const std::string& left, const std::string& right) {
    const auto a = Tags(left), b = Tags(right);
    for (const auto& tag : a) if (std::binary_search(b.begin(), b.end(), tag)) return true;
    return false;
}
inline std::vector<std::string> EnemyProfiles(const std::string& main, const std::vector<std::string>& authored) {
    std::set<std::string> profiles;
    for (const auto& source : authored) {
        const std::string profile = Canonical(source);
        // Untagged actors have no faction to join: they remain unaffiliated.
        if (!profile.empty() && !Friendly(main, profile)) profiles.insert(profile);
    }
    return {profiles.begin(), profiles.end()};
}
inline std::string UnusedPrefix(const std::vector<std::string>& authored) {
    std::string prefix = "__og_campaign_lobby_";
    for (;;) {
        bool used = false;
        for (const auto& profile : authored) for (const auto& tag : Tags(profile))
            used = used || tag.compare(0, prefix.size(), prefix) == 0;
        if (!used) return prefix;
        prefix += '_';
    }
}
inline std::map<int, std::string> Assign(int main_team, const std::set<int>& occupied,
                                       const std::string& main_tags, const std::vector<std::string>& profiles,
                                       const std::string& prefix) {
    std::map<int, std::string> result;
    size_t next = 0;
    for (int team : occupied) {
        std::string tags;
        if (team == main_team) tags = Canonical(main_tags);
        else if (next < profiles.size()) tags = Canonical(profiles[next++]);
        const std::string own = prefix + std::to_string(team);
        result[team] = tags.empty() ? own : tags + ',' + own;
    }
    // The story actor can be awaiting its seat/avatar during initial loading.
    if (!result.count(main_team)) {
        const std::string tags = Canonical(main_tags);
        result[main_team] = tags.empty() ? prefix + std::to_string(main_team) : tags + ',' + prefix + std::to_string(main_team);
    }
    return result;
}
}
