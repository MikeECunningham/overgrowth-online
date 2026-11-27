// Session progression for campaign chapters and Arena rounds. Apache-2.0.
#pragma once
#include <JSON/json.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

class MovementObject;
namespace FriendlyProgression {
struct Weapon {
    const char* key;
    const char* label;
    const char* path;
    const char* artwork;
    const char* price_family;
    int selection, rarity;
    bool offhand;
    uint64_t Ownership() const { return uint64_t{1} << selection; }
};
// Selection IDs are persistent. The array order is the order on the shop shelf.
constexpr int kWeaponCount = 36;
constexpr uint64_t kWeaponOwnershipMask = (uint64_t{1} << kWeaponCount) - 1;
const std::array<Weapon, kWeaponCount>& Weapons();
struct StatRanks {
    int health = 0, damage = 0, speed = 0;
    int stealth_passive = 0, stealth_active = 0, luck = 0;
};
struct Build : StatRanks {
    std::array<int, 9> armor{}; // front, back, head, forearms, shoulders, thighs
    int big_weapon = 0;        // zero means no main weapon
    int dagger_variant = 0;    // the original offhand dagger in older saves
    bool dagger = false;
    int ai = 0;                // Casual through Nightmare
    bool recruited = false, trained = false;
    uint32_t owner = UINT32_MAX;
};
Build GetBuild(uint32_t player);
bool ApplyRace(uint32_t player, bool bot, MovementObject& avatar);
int SurvivalShopSeconds();
bool Enabled();
bool HasHandicap(uint32_t player);
bool CanSetHandicap(uint32_t actor, uint32_t player);
bool SetHandicap(uint32_t actor, uint32_t player, bool enabled);
void Checkpoint(int checkpoint = 0, int source = -1);
void ArenaWin(const std::vector<int>& winners, bool match, const std::vector<int>* participants = nullptr);
void SurvivalClear(int battery, int wave, const std::vector<int>& winners);
void CloseSurvivalUpgrades();
void ResetAttempt();
bool BeginShop(bool campaign_final = false);
bool ShopOpen();
bool LoopChosen();
Json::Value View(uint32_t actor, uint32_t target);
bool Act(uint32_t actor, uint32_t target, const std::string& action, const std::string& key,
         unsigned expected_reward_generation = 0);

// Session, network and character hooks.
void NewSession(bool keep_slot_handicaps = false);
void NewWorld();
void ForgetPlayer(uint32_t player);
void Update();
void ReportRecovery(int victim);
void ReportHit(int victim, int attacker, float damage, bool defeat);
bool ReceiveEvent(uint8_t peer, const Json::Value& message);
bool ReceiveAction(uint32_t actor, const Json::Value& message);
Json::Value Snapshot();
bool Restore(const Json::Value& snapshot, bool validate_only = false);
}
