// Deterministic encounter equipment plans. Apache-2.0.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace CampaignEquipment {
enum Region { Front = 1, Back = 2, Head = 4, LeftForearm = 8, RightForearm = 16,
              LeftShoulder = 32, RightShoulder = 64, LeftThigh = 128, RightThigh = 256 };
constexpr int kAllRegions = 511;
enum Weapon { Unarmed = 0, Dagger = 1, Sword = 3, Spear = 4 };
struct Plan { int armor_regions = 0; Weapon weapon = Unarmed; };
inline uint32_t Mix(uint32_t value) {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
inline double Roll(uint32_t seed, uint32_t stream) { return double(Mix(seed ^ Mix(stream + 1))) / 4294967296.0; }
// Cubic onset, followed by a logarithmically slowing approach to saturation.
inline double Ease(double amount, double delay, double softness) {
    const double x = std::max(0.0, amount - delay);
    const double log = std::log1p(x * x * x);
    return log / (softness + log);
}
inline double Intensity(double effective_pressure, float weighted_allies) {
    return std::max(0.0, effective_pressure + 0.10 * std::max(0.0f, weighted_allies));
}
inline double ArmoredFraction(double intensity) { return Ease(intensity, 0.30, 0.65); }
inline double ArmedFraction(double intensity) { return Ease(intensity, 0.10, 0.45); }
// Attach matrices include inverse bind translation but no character scale.
// Retarget offsets around the reference bone anchor, not around model origin.
inline float RetargetAnchor(float position, float reference_bind, float reference_com, float reference_scale,
                            float target_bind, float target_com, float target_scale, float target_model_scale) {
    const float normalized = reference_com + (position - reference_com) / reference_scale;
    return target_com + (target_bind - target_com) * target_model_scale + (normalized - reference_bind) * target_scale;
}
inline float RetargetAnchor(float position, float reference_bind, float reference_com, float reference_scale,
                            float target_bind, float target_com, float target_scale) {
    return RetargetAnchor(position, reference_bind, reference_com, reference_scale, target_bind, target_com, target_scale, target_scale);
}
inline Plan ForUnit(uint32_t seed, double intensity, bool armor, bool weapons, bool wolf) {
    Plan plan;
    const double maturity = Ease(intensity, 0.30, 0.65);
    if (armor && Roll(seed, 0) < ArmoredFraction(intensity)) {
        // Most protected units have a few pieces, rather than nine small
        // independent rolls accidentally producing a whole army of full sets.
        const int regions[] = {Front, Back, Head, LeftForearm, RightForearm, LeftShoulder, RightShoulder, LeftThigh, RightThigh};
        const unsigned first = Mix(seed ^ 0x9e3779b9u) % 9;
        plan.armor_regions = regions[first];
        for (unsigned i = 0; i < 9; ++i)
            if (i != first && Roll(seed, 10 + i) < maturity) plan.armor_regions |= regions[i];
    }
    if (weapons && !wolf && Roll(seed, 40) < ArmedFraction(intensity)) {
        plan.weapon = Dagger;
        if (Roll(seed, 42) < Ease(intensity, 1.15, 0.45)) plan.weapon = Roll(seed, 43) < 0.5 ? Sword : Spear;
    }
    return plan;
}
inline int Pieces(int mask) { int n = 0; for (unsigned bits = unsigned(mask) & kAllRegions; bits; bits &= bits - 1) ++n; return n; }
inline uint32_t ExtraSeed(uint32_t source, uint32_t hotspot, int goal, int slot) {
    return Mix(source ^ Mix(hotspot) ^ Mix(uint32_t(goal) + 17) ^ Mix(uint32_t(slot) + 101));
}
inline int ArmedCount(int count, double intensity) {
    if (count <= 0) return 0;
    int desired = static_cast<int>(std::lround(count * ArmedFraction(intensity)));
    if (intensity >= 0.35) desired = std::max(desired, 1);
    if (intensity >= 0.75) desired = std::max(desired, 2);
    return std::min(count, desired);
}
inline Weapon WaveWeapon(uint32_t base, int slot, int count, double intensity, bool wolf) {
    if (wolf || slot < 0 || slot >= count) return Unarmed;
    int rank = 0;
    const double roll = Roll(base, 100 + slot);
    for (int i = 0; i < count; ++i) if (Roll(base, 100 + i) < roll || (Roll(base, 100 + i) == roll && i < slot)) ++rank;
    if (rank >= ArmedCount(count, intensity)) return Unarmed;
    const uint32_t seed = Mix(base ^ Mix(slot + 101));
    if (Roll(seed, 42) < Ease(intensity, 1.15, 0.45)) return Roll(seed, 43) < 0.5 ? Sword : Spear;
    return Dagger;
}
}  // namespace CampaignEquipment
