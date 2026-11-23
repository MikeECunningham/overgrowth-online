// Campaign party scaling. Apache-2.0, like the rest of the engine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace CampaignDifficulty {
enum Strength { Rat = 0, Rabbit = 1, CatOrDog = 2, BigDog = 3, Wolf = 4 };

constexpr int kRegularExtraCap = 4;
constexpr int kBossExtraCap = 2;
constexpr int kLevelExtraCap = 8;
constexpr int kActorCap = 32;

struct Factors {
    float vitality = 1.0f;
    float damage = 1.0f;
    float armor = 1.0f;
    float ai = 0.0f;
};

enum AITweak { NoTweak = 0, AggressionTweak = 1, GroundAggressionTweak = 2, FollowupTweak = 3 };
constexpr uint32_t kUnspecifiedUnit = std::numeric_limits<uint32_t>::max();

// Shared object IDs make this occasional variation identical on every peer.
// No random rolls or party-size changes may turn a unit into a new AI build.
inline AITweak TweakForUnit(uint32_t shared_id) {
    if (shared_id == kUnspecifiedUnit) return NoTweak;
    const uint32_t identity = shared_id * 1664525u + 1013904223u;
    if ((identity & 7u) != 0) return NoTweak;
    return static_cast<AITweak>(1u + ((identity >> 3u) % 3u));
}

inline float NightmareAIAmount(int participants) {
    return participants < 4 ? 0.0f : participants < 8 ? 0.01f : participants < 16 ? 0.02f : 0.03f;
}

inline float ExtraPlayers(int participants) {
    const int count = std::max(1, std::min(64, participants));
    return count <= 4 ? float(count - 1) : 3.0f + std::sqrt(float(count - 4));
}

// Preserve authored species, stats and difficulty. These are independent
// multipliers; vitality is effective health, not health bars above 100%.
inline Factors ForPlayers(int participants, bool boss, int strength, bool stats, bool nightmare,
                          uint32_t shared_id = kUnspecifiedUnit) {
    Factors out;
    const float extra = ExtraPlayers(participants);
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    if (stats) {
        const float vitality_step = (boss ? 0.65f + 0.08f * rank : 0.18f + 0.035f * rank) * 0.85f;
        out.vitality = std::min(boss ? 6.0f : 3.0f, 1.0f + vitality_step * extra);
        out.damage = std::min(boss ? 1.9f : 1.6f, 1.0f + (boss ? 0.12f : 0.075f) * 0.85f * extra);
        out.armor = std::min(boss ? 1.9f : 1.5f, 1.0f + (boss ? 0.15f : 0.07f) * 0.85f * extra);
    }
    if (nightmare && TweakForUnit(shared_id) != NoTweak) out.ai = NightmareAIAmount(participants);
    return out;
}

// Additional allies buy a small fixed number budget, independent of the
// original crowd's size. Bosses and stronger species receive additions later.
inline int ExtraEnemies(int participants, int original_count, bool boss, int strength) {
    if (participants <= 1 || original_count <= 0) return 0;
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    if (boss) {
        if (rank == Wolf) return participants >= 12 ? 1 : 0;
        if (rank == BigDog) return participants >= 8 ? 1 : 0;
        return std::min(kBossExtraCap, (participants - 1) / 5);
    }
    const int allies_per_extra = rank <= Rabbit ? 3 : 4;
    return std::min(kRegularExtraCap, (participants - 1) / allies_per_extra);
}
}  // namespace CampaignDifficulty
