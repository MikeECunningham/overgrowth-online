// Session progression for campaign chapters and Arena rounds. Apache-2.0.
#pragma once
#include <JSON/json.h>
#include <array>
#include <cstdint>
#include <string>

namespace FriendlyProgression {
struct Weapon {
    const char* key;
    const char* label;
    const char* path;
    const char* artwork;
    unsigned ownership;
    int selection, price, rarity;
};
constexpr unsigned kWeaponOwnershipMask = 31;
constexpr int kMaxMainWeapon = 4;
inline const std::array<Weapon, 5>& Weapons() {
    static const std::array<Weapon, 5> weapons = {{
        {"dagger", "Offhand dagger", "Data/Items/rabbit_weapons/rabbit_knife.xml", "Object/rabbit_knife", 1, 0, 140, 2},
        {"sword", "Sword", "Data/Items/DogWeapons/DogSword.xml", "Object/DogSword", 2, 1, 880, 3},
        {"spear", "Spear", "Data/Items/DogWeapons/DogSpear.xml", "Object/DogSpear", 4, 2, 1040, 3},
        {"staff", "Staff", "Data/Items/staffbasic.xml", "Interactive Objects/staffbasic", 8, 3, 360, 2},
        {"broadsword", "Broadsword", "Data/Items/DogWeapons/DogBroadSword.xml", "Object/DogBroadSword", 16, 4, 1280, 4},
    }};
    return weapons;
}
struct Build {
    int health = 0, damage = 0, speed = 0;
    int stealth_passive = 0, stealth_active = 0;
    std::array<int, 9> armor{}; // front, back, head, forearms, shoulders, thighs
    int big_weapon = 0;        // none, sword, spear, staff, broadsword
    bool dagger = false;
    int ai = 0;                // Casual through Nightmare
    bool recruited = false, trained = false;
    uint32_t owner = UINT32_MAX;
};
Build GetBuild(uint32_t player);
bool Enabled();
void Checkpoint();
void ResetAttempt();
bool BeginShop(bool campaign_final = false);
bool ShopOpen();
bool LoopChosen();
Json::Value View(uint32_t actor, uint32_t target);
bool Act(uint32_t actor, uint32_t target, const std::string& action, const std::string& key);

// Session, network and character hooks.
void NewSession();
void NewWorld();
void ForgetPlayer(uint32_t player);
void Update();
void ReportHit(int victim, int attacker, float damage, bool defeat);
bool ReceiveEvent(uint8_t peer, const Json::Value& message);
bool ReceiveAction(uint32_t actor, const Json::Value& message);
Json::Value Snapshot();
bool Restore(const Json::Value& snapshot, bool validate_only = false);
}
