// Campaign party scaling. Apache-2.0, like the rest of the engine.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace CampaignDifficulty {
// Sampled by the host at a level, checkpoint or Arena match boundary.
struct Sample {
    int humans = 0, bots = 0; // zero humans means the level has not sampled yet
    float pressure = 0, base = -1; // -1 uses the ordinary live base preset
    bool stats = false, ai = false, armor = false, weapons = false;
};

enum Strength { Rat = 0, Rabbit = 1, CatOrDog = 2, BigDog = 3, Wolf = 4 };

constexpr int kRegularExtraCap = 6;
constexpr int kBossExtraCap = 2;
constexpr int kConcurrentExtraCap = 8;
constexpr int kActorCap = 32;
constexpr float kHumanAllyWeight = 1.4f;
constexpr float kBotAllyWeight = 0.35f;

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

inline float HumanAIAmount(int participants) {
    return participants < 4 ? 0.0f : participants < 8 ? 0.01f : participants < 16 ? 0.02f : 0.03f;
}
inline float AIAmount(int humans, double pressure) {
    return std::max(HumanAIAmount(humans), pressure < 0.6 ? 0.0f : pressure < 1.1 ? 0.01f : pressure < 1.5 ? 0.02f : 0.03f);
}

inline float ExtraPlayers(int participants) {
    const int count = std::max(1, std::min(64, participants));
    return count <= 4 ? float(count - 1) : 3.0f + std::sqrt(float(count - 4));
}

// Fixed twentieths keep mixed-party stat weights exact. The main
// human is the unchanged baseline; each additional human weighs four bots.
inline int PartyAllyUnits(int humans, int bots) {
    const int human_allies = std::max(1, std::min(64, humans)) - 1;
    const int bot_allies = std::max(0, std::min(64, bots));
    return std::min(63 * 20, human_allies * 28 + bot_allies * 7);
}

inline float EffectivePartySize(int humans, int bots) {
    return 1.0f + float(PartyAllyUnits(humans, bots)) / 20.0f;
}

inline float ExtraPlayersForParty(int humans, int bots) {
    const float allies = float(PartyAllyUnits(humans, bots)) / 20.0f;
    return allies <= 3.0f ? allies : 3.0f + std::sqrt(allies - 3.0f);
}

// Preserve authored species, stats and difficulty. These are independent
// multipliers; vitality is effective health, not health bars above 100%.
inline Factors ForExtraPlayers(float extra, int ai_humans, bool boss, int strength, bool stats, bool ai_enabled,
                              uint32_t shared_id) {
    Factors out;
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    if (stats) {
        const float vitality_step = (boss ? 0.65f + 0.08f * rank : 0.18f + 0.035f * rank) * 0.85f;
        out.vitality = std::min(boss ? 6.0f : 3.0f, 1.0f + vitality_step * extra);
        out.damage = std::min(boss ? 1.9f : 1.6f, 1.0f + (boss ? 0.12f : 0.075f) * 0.85f * extra);
        // Armor is supplied as real pieces, not a passive-block multiplier.
    }
    if (ai_enabled && TweakForUnit(shared_id) != NoTweak) out.ai = HumanAIAmount(ai_humans);
    return out;
}

// Legacy callers retain the original unweighted participant model.
inline Factors ForPlayers(int participants, bool boss, int strength, bool stats, bool ai_enabled,
                          uint32_t shared_id = kUnspecifiedUnit) {
    return ForExtraPlayers(ExtraPlayers(participants), participants, boss, strength, stats, ai_enabled, shared_id);
}

inline Factors ForParty(int humans, int bots, bool boss, int strength, bool stats, bool ai_enabled,
                        uint32_t shared_id = kUnspecifiedUnit) {
    return ForExtraPlayers(ExtraPlayersForParty(humans, bots), humans, boss, strength, stats, ai_enabled, shared_id);
}

// Counts grow eagerly, then logarithmically. Unlike stat scaling, another
// human contributes two bots here. Round only after the species/boss factor.
inline int ExtraEnemiesForParty(int humans, int bots, int original_count, bool boss, int strength) {
    if (original_count <= 0) return 0;
    const int human_allies = std::max(1, std::min(64, humans)) - 1;
    const int bot_allies = std::max(0, std::min(64, bots));
    const double allies = double(human_allies) + double(bot_allies) * 0.5;
    const double growth = 2.0 * std::log2(1.0 + allies * 0.5);
    const int rank = std::max(int(Rat), std::min(int(Wolf), strength));
    const double rate = boss ? (rank == Wolf ? 0.25 : rank == BigDog ? 0.30 : 0.40) : (rank <= Rabbit ? 1.0 : 0.85);
    const int cap = boss ? (rank >= BigDog ? 1 : kBossExtraCap) : kRegularExtraCap;
    return std::min(cap, static_cast<int>(std::lround(growth * rate)));
}
}  // namespace CampaignDifficulty
