// Physical weapons with interchangeable cosmetic appearances.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace WeaponCategories {
struct Skin {
    std::string id, label, artwork, path;
    int rarity = 0;  // common, uncommon, rare, epic, legendary
};
struct Category {
    std::string id, label, price_family;
    int rarity = 0;
    bool offhand = false;
    std::vector<Skin> skins;  // mundane first, then roll order
};

const std::vector<Category>& Catalog();
const Category* Find(const std::string& id);
const Category* ForItem(const std::string& path, const Skin** skin = nullptr);
const Skin& Appearance(const Category& category, const std::string& skin);
void Invalidate();

// Stable conversions for saves made when every physical variant was a separate purchase.
constexpr int kLegacyCount = 36;
std::string FromLegacy(int selection);
int ToLegacy(const std::string& category);
std::string OldPurchase(const std::string& key);
std::string OldItemPath(const std::string& key);
}
