// Prices and rewards use XP. A normal rabbit pays for one starting stat upgrade.
#pragma once
#include <Online/armor_protection.h>
#include <algorithm>
#include <cmath>

namespace FriendlyProgression {
inline double Bound(double x, double low, double high) { return std::max(low, std::min(high, x)); }
constexpr int kMaxRank = 50;
constexpr int kMinimumGainRank = 20;
struct RankGains {
    double first, minimum;

    double Next(int owned) const {
        const int remaining = std::max(0, kMinimumGainRank - 1 - std::max(0, owned));
        return minimum + (first - minimum) * remaining / (kMinimumGainRank - 1);
    }
    double Total(int rank) const {
        rank = std::max(0, rank);
        const int taper_ranks = std::min(rank, kMinimumGainRank);
        const double average_gain = (first + Next(taper_ranks - 1)) * 0.5;
        return taper_ranks * average_gain + (rank - taper_ranks) * minimum;
    }
};
constexpr RankGains kDurabilityGain{.16, .04}, kDamageGain{.12, .03}, kSpeedGain{.06, .015};
constexpr RankGains kStealthGain{.07, .0025}, kLuckGain{.04, .01};

inline float DurabilityMultiplier(int rank) { return static_cast<float>(1.0 + kDurabilityGain.Total(rank)); }
inline float DamageMultiplier(int rank) { return static_cast<float>(1.0 + kDamageGain.Total(rank)); }
inline float FallDamageMultiplier(int rank) { return 1.0f / DamageMultiplier(rank); }
inline float SpeedMultiplier(int rank) { return static_cast<float>(1.0 + kSpeedGain.Total(rank)); }
inline float StealthRangeMultiplier(int rank) { return static_cast<float>(std::max(0.0, 1.0 - kStealthGain.Total(rank))); }
inline int StatPrice(int rank, double base, double linear, double quadratic) {
    return static_cast<int>(std::round(base + linear * rank + quadratic * rank * rank));
}
inline int ArmorPrice(unsigned region, int material) {
    const int base[] = {300, 245, 215, 80, 80, 125, 125, 100, 100};
    static constexpr int multiplier[] = {0, 1, 2, 3, 5};
    return base[region] * multiplier[std::min(int(ArmorProtection::Grey), std::max(0, material))];
}
inline int RecruitPrice(int bought, double base, double growth) {
    return static_cast<int>(std::min(50000000.0, std::round(base * std::pow(growth, std::min(bought, 15)))));
}
inline double FreeRewardChance(int luck) {
    return .20 + kLuckGain.Total(std::min(luck, kMaxRank));
}
// An upgrade priced at the budget has half the weight of a very cheap upgrade.
// At 20 Luck that includes a first recruit at the mod's current price.
inline double FreeRewardBudget(int luck, double first_recruit_price) {
    constexpr double starting_budget = 100.0;
    const double progress = kLuckGain.Total(std::min(luck, kMaxRank)) / kLuckGain.Total(20);
    return starting_budget + std::max(0.0, first_recruit_price - starting_budget) * progress;
}
inline double FreeRewardWeight(int price, double budget) {
    return 1.0 / (1.0 + std::pow(price / budget, 6));
}
inline double Reward(double bounty, double difficulty, double equipment,
                     double durability, double damage, double pressure) {
    double xp = bounty * (0.85 + 0.30 * Bound(difficulty, 0.0, 1.0));
    xp *= 1.0 + Bound(equipment, 0.0, 2.5);
    xp *= std::sqrt(Bound(durability * damage, 0.35, 12.0)) * (1.0 + 0.20 * Bound(pressure, -0.25, 1.5));
    return std::round(xp);
}
struct Wallet {
    // Double-backed balances can distinguish every whole XP through 2^53 - 1.
    static constexpr double kMaxExactXP = 9007199254740991.0;
    double held = 0, pooled = 0, commit_amount = 0;
    unsigned commit_serial = 0;
    void Earn(double amount, bool bank) {
        if (!std::isfinite(amount)) return;
        auto& balance = bank ? held : pooled;
        balance = Bound(balance + amount, 0.0, kMaxExactXP);
    }
    void Lose(double cost, bool use_committed) {
        if (cost <= 0 || !std::isfinite(cost)) return;
        const double from_pool = std::min(pooled, cost);
        pooled -= from_pool;
        if (use_committed) held = std::max(0.0, held - (cost - from_pool));
    }
    void Commit() {
        if (pooled <= 0) return;
        commit_amount = pooled;
        Earn(pooled, true);
        pooled = 0;
        ++commit_serial;
    }
};
}
