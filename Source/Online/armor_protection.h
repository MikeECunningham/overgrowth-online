// Shared armor rules for authored equipment and progression purchases. Apache-2.0.
#pragma once

#include <algorithm>
#include <array>
#include <string>

class Object;

namespace ArmorProtection {
void ApplyMaterial(Object& mesh, int tier);
enum Material { Wood = 1, Chitin = 2, Dog = 3, Grey = 4,
                Leather = Wood, Reinforced = Chitin, Metal = Dog };
// The stock wearable supplies mirrored shoulder, lower-arm and thigh poses.
inline bool HasWoodenGuard(unsigned region) { return region >= 3 && region < 9; }
inline int NextShopMaterial(unsigned region, int current) {
    if (region >= 9 || current >= Grey) return 0;
    return current == 0 ? (HasWoodenGuard(region) ? Wood : Chitin) : current + 1;
}
// Alpha.12 used synthetic leather/reinforced finishes on metal meshes.
// Preserve unlocked coverage when restoring a pre-catalogue session.
inline int RestoredShopMaterial(unsigned region, int material, bool legacy = true) {
    if (!legacy) return material == Wood && !HasWoodenGuard(region) ? Chitin : material;
    if (material == 0 || material == Dog) return material;
    return HasWoodenGuard(region) ? Wood : Dog;
}
inline const char* MaterialName(int material) {
    const char* names[] = {"", "Wood", "Chitin", "Dog armor", "Therium grey"};
    return names[std::min(int(Grey), std::max(0, material))];
}
using Coverage = std::array<int, 9>;

// Base weights are ratings, not percentages. The best piece at a location
// counts once, even if its model uses several attached decorative objects.
inline float Rating(const Coverage& pieces) {
    static constexpr float weight[] = {.28f, .20f, .30f, .065f, .065f, .09f, .09f, .095f, .095f};
    static constexpr float strength[] = {0, .50f, .85f, 1.30f, 1.75f};
    float rating = 0;
    for (unsigned i = 0; i < pieces.size(); ++i)
        rating += weight[i] * strength[std::min(int(Grey), std::max(0, pieces[i]))];
    return rating;
}

inline int MaterialTier(const std::string& name, int fallback = Dog) {
    if (name.find("chitinarmour/") != std::string::npos) return Chitin;
    if (name.find("dog_armour_grey/") != std::string::npos) return Grey;
    if (name.find("dog_armor/") != std::string::npos) return Dog;
    if (name.find("wood") != std::string::npos || name.find("bone") != std::string::npos ||
        name.find("reinforced") != std::string::npos) return Wood;
    if (name.find("leather") != std::string::npos || name.find("cloth") != std::string::npos) return Wood;
    return fallback;
}
}
