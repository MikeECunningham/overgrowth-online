// Prices and rewards use XP. A normal rabbit pays for one starting stat upgrade.
#pragma once
#include <Online/armor_protection.h>
#include <algorithm>
#include <cmath>

namespace FriendlyProgression {
inline double Bound(double x, double low, double high) { return std::max(low, std::min(high, x)); }
constexpr int kMaxRank = 50;
inline int StatPrice(int rank) { return 16 + 5 * rank + 2 * rank * rank; }
inline int ArmorPrice(unsigned region, int material) {
    const int base[] = {300, 245, 285, 155, 155, 195, 195, 175, 175};
    static constexpr int multiplier[] = {0, 1, 2, 3, 5};
    return base[region] * multiplier[std::min(int(ArmorProtection::Grey), std::max(0, material))];
}
inline int RecruitPrice(int bought) {
    return static_cast<int>(std::min(50000000.0, std::round(2400.0 * std::pow(2.25, std::min(bought, 15)))));
}
inline double Reward(int species, bool large_dog, double difficulty, double equipment,
                     double durability, double damage, double pressure) {
    // AngelScript species enum: rabbit, wolf, dog, rat, cat.
    const double base[] = {16, 48, 24, 10, 24};
    double xp = large_dog ? 34 : base[std::max(0, std::min(4, species))];
    xp *= (0.85 + 0.30 * Bound(difficulty, 0.0, 1.0));
    xp *= 1.0 + Bound(equipment, 0.0, 2.5);
    xp *= std::sqrt(Bound(durability * damage, 0.35, 12.0)) * (1.0 + 0.20 * Bound(pressure, -0.25, 1.5));
    return std::round(xp);
}
struct Wallet {
    double held = 0, pooled = 0, commit_amount = 0;
    unsigned commit_serial = 0;
    void Earn(double amount, bool bank) {
        auto& balance = bank ? held : pooled;
        balance = Bound(balance + amount, 0.0, 100000000.0);
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
