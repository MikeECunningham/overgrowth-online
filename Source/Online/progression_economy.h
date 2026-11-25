// Prices and rewards use XP. A normal rabbit pays for one starting stat upgrade.
#pragma once
#include <Online/armor_protection.h>
#include <algorithm>
#include <cmath>

namespace FriendlyProgression {
inline double Bound(double x, double low, double high) { return std::max(low, std::min(high, x)); }
constexpr int kMaxRank = 50;
inline int StatPrice(int rank, double base, double linear, double quadratic) {
    return static_cast<int>(std::round(base + linear * rank + quadratic * rank * rank));
}
inline int ArmorPrice(unsigned region, int material) {
    const int base[] = {300, 245, 285, 155, 155, 195, 195, 175, 175};
    static constexpr int multiplier[] = {0, 1, 2, 3, 5};
    return base[region] * multiplier[std::min(int(ArmorProtection::Grey), std::max(0, material))];
}
inline int RecruitPrice(int bought, double base, double growth) {
    return static_cast<int>(std::min(50000000.0, std::round(base * std::pow(growth, std::min(bought, 15)))));
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
    void Commit() {
        if (pooled <= 0) return;
        commit_amount = pooled;
        Earn(pooled, true);
        pooled = 0;
        ++commit_serial;
    }
};
}
