// Campaign party scaling. Apache-2.0, like the rest of the engine.
#pragma once

#include <algorithm>
#include <cmath>

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

inline float ExtraPlayers(int humans) {
    const int count = std::max(1, std::min(64, humans));
    return count <= 4 ? float(count - 1) : 3.0f + std::sqrt(float(count - 4));
}

// Preserve authored species, stats and difficulty. These are independent
// multipliers; vitality is effective health, not health bars above 100%.
inline Factors ForPlayers(int humans, bool boss, int strength, bool stats, bool nightmare) {
    Factors out;
    const float extra = ExtraPlayers(humans);
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    if (stats) {
        const float vitality_step = boss ? 0.65f + 0.08f * rank : 0.18f + 0.035f * rank;
        out.vitality = std::min(boss ? 6.0f : 3.0f, 1.0f + vitality_step * extra);
        out.damage = std::min(boss ? 1.9f : 1.6f, 1.0f + (boss ? 0.12f : 0.075f) * extra);
        out.armor = std::min(boss ? 1.9f : 1.5f, 1.0f + (boss ? 0.15f : 0.07f) * extra);
    }
    if (nightmare) out.ai = std::min(0.35f, extra * (boss ? 0.085f : 0.07f));
    return out;
}

// Weak-species encounters gain numbers earlier. Bosses primarily gain
// durability; large dogs and wolves do not become a pack at two players.
inline int ExtraEnemies(int humans, int original_count, bool boss, int strength) {
    if (humans <= 1 || original_count <= 0) return 0;
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    if (boss) {
        if (rank == Wolf) return humans >= 8 ? 1 : 0;
        if (rank == BigDog) return humans >= 6 ? 1 : 0;
        return std::min(kBossExtraCap, (humans - 1) / 3);
    }
    const float density[] = {0.42f, 0.32f, 0.24f, 0.16f, 0.10f};
    const double count = double(std::min(original_count, kActorCap)) * density[rank] * ExtraPlayers(humans);
    return std::min(kRegularExtraCap, int(std::floor(count + 0.5)));
}
}  // namespace CampaignDifficulty
