// Friendly progression is owned by the host. Combat peers report outcomes only
// for victims they simulate, and shops transact against the host's wallet.
#include <Online/local_profiles.h>
#include <Online/friendly_progression.h>
#include <Online/weapon_categories.h>
#include <Online/session_mods.h>
#include <Online/armor_protection.h>
#include <Online/progression_economy.h>
#include <Online/campaign_difficulty.h>
#include <Online/online.h>
#include <Online/online_utility.h>
#include <Asset/Asset/actorfile.h>
#include <Asset/Asset/character.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Editors/map_editor.h>
#include <Objects/movementobject.h>
#include <Objects/itemobject.h>
#include <Internal/config.h>
#include <Internal/modloading.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace FriendlyProgression {

namespace {
using ID = uint32_t;
constexpr ID invalid = UINT32_MAX;
const char* const regions[] = {"Chest", "Back", "Head", "Left forearm", "Right forearm", "Left shoulder", "Right shoulder", "Left thigh", "Right thigh"};
const char* const training_names[] = {"Hardcore AI", "Expert AI", "Nightmare AI"};
const int timers[] = {0, 10, 15, 30, 60, 120};
struct RaceTier {
    const char* name;
    const char* species;
    const char* character;
    double recruit_price_multiplier;
};
constexpr RaceTier races[] = {
    {"Mouse", "rat", "Data/Characters/rat.xml", 0},
    {"Rabbit", "rabbit", "Data/Characters/turner.xml", 0.5},
    {"Cat", "cat", "Data/Characters/male_cat.xml", 1.0},
    {"Dog", "dog", "Data/Characters/dog_soldier.xml", 4.0},
    {"Wolf", "wolf", "Data/Characters/male_wolf.xml", 16.0}
};
struct Account {
    Wallet wallet;
    Build build;
    int race = -1;           // Highest purchased tier. Before purchase the fighter keeps its starting body.
    int first_race = -1;     // Starting tier when this unit first upgraded.
    int equipped_race = -1;  // A lower owned tier, or -1 to wear the highest.
    std::set<std::string> weapons;
    bool handicap = false;
    // Checkpoints may arrive while this player is choosing an earlier hand.
    std::map<unsigned, std::vector<std::string>> reward_hands;
};
struct Team : StatRanks {
    Wallet wallet;
    ID leader = invalid, last_recipient = invalid;
    int recruits = 0;
};
struct Run {
    bool restrict_upgrades = false;
    std::set<std::string> map_upgrades;
    // The cursor is saved with the run. Profile claims stay on the host even
    // when an older match save is loaded again.
    std::string profile_run_id, profile_event;
    unsigned profile_completion = 0;
    std::map<ID, Account> players;
    std::map<int, Team> teams;
    unsigned shop_generation = 0, reward_generation = 0, attempt = 0;
    unsigned completed_rounds = 0;
    std::map<int, int> checkpoints;
    bool open = false, final = false, loop = false;
    bool round_completed = false, match_completed = false;
    int survival_battery = 0;
    std::set<std::pair<int, int>> survival_clears; // (team, wave), with (0, 0) for the final clear
    double deadline_ms = 0;
    std::set<ID> ready;
    std::map<ID, Json::Value> xp_scales;
    std::map<int, std::string> sold_authored;

    void CompleteRound() {
        if (completed_rounds < UINT32_MAX) ++completed_rounds;
    }
};
Run run;
bool restore_sales = false;
// A remote owner may already be down or disconnected by the time its heal
// arrives. Reserve the price or hand until that specific body confirms it.
struct PendingRestore {
    ID buyer;
    int team_wallet, object;
    double cost;
    unsigned hand;
    bool phase, rotate;
};
std::map<ID, PendingRestore> pending_restores;
std::map<ID, unsigned> last_action;
unsigned next_action = 0;
bool dirty = false;
double next_send = 0, next_sample = 0;
std::map<ID, std::pair<double, Json::Value>> stats_cache;

struct Spawn {
    MovementObject* actor = nullptr;
    double equipment = 0, sample_until = 0;
    bool engaged = false, defeated = false;
};
std::map<int, Spawn> spawns;
Online* Net() { return Online::Instance(); }
bool Arena() { return Net()->LobbyMode() == "com-wolfire-arena" || Net()->LobbyCampaign() == "com-wolfire-arena"; }
bool PaidShop() { return Enabled() && Net()->ModeSetting("progression_shop") != 0; }
bool FreeBuffs() { return Net()->ModeSetting("progression_free_buffs") != 0; }
bool Shared() { return Net()->ModeSetting("progression_shared") != 0; }

const PlayerState* Seat(ID id) {
    if (!Net()->online_session) return nullptr;
    const auto& players = Net()->online_session->player_states;
    auto it = players.find(id);
    return it == players.end() ? nullptr : &it->second;
}
MovementObject* Actor(int id) {
    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    Object* obj = graph && id >= 0 ? graph->GetObjectFromID(id) : nullptr;
    return obj && obj->GetType() == _movement_object ? static_cast<MovementObject*>(obj) : nullptr;
}
int NaturalRace(MovementObject& actor) {
    if (!actor.as_context || !actor.ASHasVar("species")) return -1;
    const int species = actor.ASGetIntVar("species");
    constexpr int order[] = {1, 4, 3, 0, 2};  // rabbit, wolf, dog, rat, cat in aschar
    return species >= 0 && species < 5 ? order[species] : -1;
}
int StartingRace(bool bot) {
    if (bot && (Enabled() || Net()->IsSurvivalMode())) return 1;
    if (Net()->IsSurvivalMode()) return std::min(4, std::max(0, Net()->ModeSetting("survival_starting_race")));
    return -1;
}
int HighestRace(const PlayerState& seat) {
    const auto found = run.players.find(seat.player_id);
    if (found != run.players.end() && found->second.race >= 0) return found->second.race;
    const int start = StartingRace(seat.is_bot);
    if (start >= 0) return start;
    auto* actor = Actor(seat.object_id);
    return actor ? NaturalRace(*actor) : -1;
}
// The original actor resource survives rig swaps, including on an older
// save that recorded only the highest purchased tier.
int AuthoredRace(MovementObject& avatar, std::string* path = nullptr) {
    if (avatar.obj_file.empty()) return -1;
    auto* assets = Engine::Instance()->GetAssetManager();
    auto actor = assets->LoadSync<ActorFile>(avatar.obj_file);
    if (!actor.valid() || actor->character.empty()) return -1;
    auto character = assets->LoadSync<Character>(actor->character);
    if (!character.valid()) return -1;
    if (path) *path = actor->character;
    const auto& species = character->GetTag("species");
    for (int tier = 0; tier < 5; ++tier) if (species == races[tier].species) return tier;
    return -1;
}
int LegacyFirstRace(bool bot, int highest, MovementObject& avatar) {
    int start = StartingRace(bot);
    if (start < 0) start = AuthoredRace(avatar);
    // A recorded purchase necessarily started below its final tier.
    return std::min(start < 0 ? 1 : start, std::max(0, highest - 1));
}
int ShopTeam(const PlayerState& seat) {
    if (!Arena() && !Net()->IsSurvivalMode() && !Net()->LobbyForceTeams()) return 1;
    return seat.team;
}
std::vector<ID> Members(int team, bool include_bots = false) {
    std::vector<ID> result;
    for (const auto& row : Net()->online_session->player_states) {
        const auto& p = row.second;
        if (ShopTeam(p) == team && (include_bots || !p.is_bot) && (p.connected || p.is_bot)) result.push_back(row.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}
ID Leader(int team) {
    const auto people = Members(team);
    auto& elected = run.teams[team].leader;
    if (std::find(people.begin(), people.end(), elected) != people.end()) return elected;
    const ID candidate = people.empty() ? invalid : people.front();
    if (Net()->IsHosting()) elected = candidate;
    return candidate;
}
bool Alive(const PlayerState& p) {
    auto* actor = Actor(p.object_id);
    return actor && actor->enabled_ && actor->as_context && actor->ASHasVar("knocked_out") && actor->ASGetIntVar("knocked_out") == 0;
}
Wallet& Balance(const PlayerState& p) {
    return Shared() && !p.is_bot ? run.teams[ShopTeam(p)].wallet : run.players[p.player_id].wallet;
}
double Spendable(const PlayerState& buyer) {
    const int team = Shared() && !buyer.is_bot ? ShopTeam(buyer) : 0;
    double xp = Balance(buyer).held;
    for (const auto& entry : pending_restores) {
        const auto& restore = entry.second;
        if (restore.team_wallet == team && (team || restore.buyer == buyer.player_id)) xp -= restore.cost;
    }
    return xp;
}
void Publish() {
    if (!Net()->IsHosting()) return;
    SessionMods::PublishFeature("progression");
    dirty = false;
    next_send = OnlineClockMs() + 150;
}
double SpawnEquipment(MovementObject* actor) {
    if (!actor->rigged_object()) return 0;
    double value = Net()->WornArmorRating(actor->GetID());
    std::set<int> held;
    for (const auto& attachment : actor->rigged_object()->attached_items.items) {
        const auto* item = attachment.item.GetAttached();
        if (!item || !held.insert(item->GetID()).second) continue;
        const auto& asset = item->item_ref();
        if (asset->GetItemType() == _weapon) value += asset->GetLabel() == "knife" ? .18 : .38;
    }
    return value;
}
Spawn& Observe(MovementObject* actor) {
    auto& spawn = spawns[actor->GetID()];
    if (spawn.actor != actor) {
        spawn = Spawn{};
        spawn.actor = actor;
        spawn.sample_until = OnlineClockMs() + 1800;
        spawn.equipment = SpawnEquipment(actor);
    } else if (!spawn.engaged && OnlineClockMs() <= spawn.sample_until) {
        // Allow authored attachment messages and the director's spawn queue to
        // settle. Disarming the opponent during a fight never lowers its award.
        spawn.equipment = std::max(spawn.equipment, SpawnEquipment(actor));
    }
    return spawn;
}
double Rule(const std::string& key) {
    const auto* provider = ModLoading::Instance().GetSessionFeature("progression");
    return provider ? provider->Rule(key) : 0;
}
int UpgradePrice(int rank) {
    return StatPrice(rank, Rule("stat_base"), Rule("stat_linear"), Rule("stat_quadratic"));
}
double BaseBounty(int species, bool large_dog) {
    const char* const keys[] = {"bounty_rabbit", "bounty_wolf", "bounty_dog", "bounty_rat", "bounty_cat"};
    return Rule(large_dog ? "bounty_large_dog" : keys[std::max(0, std::min(4, species))]);
}
double Bounty(MovementObject* victim, double gear) {
    const int species = victim->ASHasVar("species") ? victim->ASGetIntVar("species") : 0;
    const bool large = species == 2 && victim->character_path.find("dog_big") != std::string::npos;
    const auto sample = Net()->EnemyDifficultyFor(victim->GetID());
    auto factor = [&](const char* name) { return victim->ASHasVar(name) ? victim->ASGetFloatVar(name) : 1.f; };
    return Reward(BaseBounty(species, large), sample.base >= 0 ? sample.base : config["game_difficulty"].toNumber<double>(), gear,
        factor("campaign_vitality_mult"), factor("campaign_damage_mult"), sample.pressure);
}
void SampleXPScales(SceneGraph* graph) {
    const int kills = Net()->ModeSetting("progression_gain_kill");
    const int damage = Net()->ModeSetting("progression_gain_damage");
    const bool survival = Net()->IsSurvivalMode();
    const auto sample = Net()->EnemyDifficulty();
    for (const auto& row : Net()->online_session->player_states) {
        const auto& player = row.second;
        if (player.is_bot || !player.connected) continue;
        MovementObject* hero = Actor(player.object_id);
        if (!hero || !hero->as_context) continue;
        double world = 0, encounter = 0;
        auto enemy = [&](MovementObject* actor) {
            if (!actor || actor == hero || !actor->as_context || !actor->ASHasVar("species") || !actor->ASHasVar("knocked_out")) return false;
            if (survival) {
                auto* params = actor->GetScriptParams();
                return actor->enabled_ && params->HasParam("Survival Team") && params->ASGetInt("Survival Team") == player.team;
            }
            return !hero->ASOnSameTeam(actor);
        };
        for (Object* object : graph->movement_objects_) {
            auto* actor = static_cast<MovementObject*>(object);
            if (!enemy(actor) || actor->ASGetIntVar("knocked_out") != 0) continue;
            auto& spawn = Observe(actor);
            if (!spawn.defeated) {
                double remaining = 1.0;
                for (const char* key : {"temp_health", "blood_health"})
                    if (actor->ASHasVar(key)) remaining = std::min(remaining, double(actor->ASGetFloatVar(key)));
                world += Bounty(actor, spawn.equipment) * (kills + damage * std::max(0.0, remaining));
            }
        }
        const auto scope = Net()->ProgressionEncounterScope(hero->GetID());
        std::set<int> counted;
        for (const auto& id : scope["actors"]) {
            MovementObject* actor = Actor(id.asInt());
            if (enemy(actor) && counted.insert(actor->GetID()).second)
                encounter += Bounty(actor, Observe(actor).equipment);
        }
        for (const auto& pending : scope["pending"]) {
            MovementObject* source = Actor(pending["source"].asInt());
            if (!enemy(source)) continue;
            const int species = source->ASGetIntVar("species");
            const bool large = species == 2 && source->character_path.find("dog_big") != std::string::npos;
            encounter += Reward(BaseBounty(species, large), sample.base >= 0 ? sample.base :
                config["game_difficulty"].toNumber<double>(), pending["equipment"].asDouble(),
                pending["vitality"].asDouble(), pending["damage"].asDouble(), sample.pressure);
        }
        Json::Value scale;
        scale["world_remaining"] = world;
        scale["encounter_expected"] = encounter * (kills + damage);
        scale["encounter_label"] = scope.get("label", "Current encounter").asString();
        if (run.xp_scales[player.player_id] != scale) {
            run.xp_scales[player.player_id] = scale;
            dirty = true;
        }
    }
}
Json::Value PlayerStats(const PlayerState& player) {
    auto& cache = stats_cache[player.player_id];
    const double now = OnlineClockMs();
    if (now < cache.first && !cache.second.isNull()) return cache.second;
    cache.first = now + 200;
    Json::Value stats;
    const auto build = GetBuild(player.player_id);
    stats["player"] = player.player_id;
    stats["name"] = player.playername;
    stats["health_rank"] = build.health; stats["speed_rank"] = build.speed;
    stats["bone_damage_rank"] = build.bone_damage; stats["flesh_damage_rank"] = build.flesh_damage;
    stats["fall_resistance_rank"] = build.fall_resistance;
    stats["fall_resistance"] = 1.0f - FallDamageMultiplier(build.fall_resistance);
    stats["stealth_passive_rank"] = build.stealth_passive; stats["stealth_active_rank"] = build.stealth_active;
    if (!player.is_bot && FreeBuffs()) {
        stats["luck_rank"] = build.luck;
        stats["reward_chance"] = FreeRewardChance(build.luck);
    }
    stats["passive_range"] = StealthRangeMultiplier(build.stealth_passive);
    stats["active_range"] = StealthRangeMultiplier(build.stealth_active);
    MovementObject* actor = Actor(player.object_id);
    auto value = [&](const char* key, double fallback) {
        return actor && actor->ASHasVar(key) ? double(actor->ASGetFloatVar(key)) : fallback;
    };
    stats["durability"] = 1.0 / std::max(.00001, value("p_damage_multiplier", 1));
    const double attack = value("p_attack_damage_mult", 1);
    stats["bone_damage"] = attack * DamageMultiplier(build.bone_damage);
    stats["flesh_damage"] = attack * DamageMultiplier(build.flesh_damage);
    stats["movement"] = value("p_speed_mult", 1) * SpeedMultiplier(build.speed);
    auto armor = Net()->WornArmorCoverage(player.object_id);
    if (run.open) for (unsigned region = 0; region < armor.size(); ++region)
        if (build.armor[region]) armor[region] = build.armor[region];
    stats["armor_rating"] = run.open && std::any_of(armor.begin(), armor.end(), [](int tier) { return tier > 0; }) ?
        ArmorProtection::Rating(armor) : Net()->WornArmorRating(player.object_id);
    stats["armor_reduction"] = 1.0 - 1.0 / (1.0 + stats["armor_rating"].asDouble());
    stats["armor_mode"] = config["armor_localized"].toNumber<int>();
    stats["armor"] = Json::Value(Json::arrayValue);
    for (int tier : armor) stats["armor"].append(tier);
    cache.second = stats;
    return stats;
}
bool BankCombatXPImmediately() {
    return (Arena() || Net()->IsSurvivalMode()) && !Net()->ModeSetting("progression_loss_damage") && !Net()->ModeSetting("progression_loss_death");
}
void CommitPooledXP() {
    for (auto& entry : run.players) entry.second.wallet.Commit();
    for (auto& entry : run.teams) entry.second.wallet.Commit();
    dirty = true;
}
// Nightmare bots keep a personal training wallet even when their team shares XP.
Wallet* PersonalXPWallet(const PlayerState& seat) {
    if (seat.is_bot ? run.players[seat.player_id].build.ai != 3 : Shared()) return nullptr;
    return &run.players[seat.player_id].wallet;
}
template <typename Action>
void ForEachSeatWallet(const PlayerState& seat, Action apply) {
    if (Shared()) apply(run.teams[ShopTeam(seat)].wallet);
    if (Wallet* wallet = PersonalXPWallet(seat)) apply(*wallet);
}
void RewardKill(const PlayerState& killer, MovementObject* fighter, double reward) {
    const int team = ShopTeam(killer);
    const bool bank = BankCombatXPImmediately();
    if (Shared()) run.teams[team].wallet.Earn(reward, bank);
    for (ID id : Members(team, true)) {
        const auto& p = *Seat(id);
        if (!Alive(p)) continue;
        auto* actor = Actor(p.object_id);
        if (!actor->ASOnSameTeam(fighter)) continue; // Arena can stage FFA among lobby friends.
        if (Wallet* wallet = PersonalXPWallet(p)) wallet->Earn(reward, bank);
    }
}
void RewardRecipients(const std::vector<const PlayerState*>& recipients, double reward, bool bank) {
    std::set<int> paid_teams;
    for (const auto* seat : recipients) {
        const int team = ShopTeam(*seat);
        if (Shared() && paid_teams.insert(team).second) run.teams[team].wallet.Earn(reward, bank);
        if (Wallet* wallet = PersonalXPWallet(*seat)) wallet->Earn(reward, bank);
    }
    dirty = true;
}

void ApplyHit(MovementObject* victim, MovementObject* attacker, float damage, bool defeat) {
    if (!victim || !victim->as_context) return;
    auto& spawn = Observe(victim);
    if (spawn.defeated) return;
    spawn.engaged = true;
    spawn.defeated = defeat;
    const bool hostile = attacker && attacker->as_context && attacker != victim && !victim->ASOnSameTeam(attacker);
    PlayerState seat;
    if (hostile && Net()->TryGetPlayerState(seat, attacker->GetID())) {
        if (defeat && Net()->ModeSetting("progression_gain_kill")) {
            RewardKill(seat, attacker, Bounty(victim, spawn.equipment));
        } else if (!defeat && Net()->ModeSetting("progression_gain_damage")) {
            const double reward = Bounty(victim, spawn.equipment) * damage;
            const bool bank = BankCombatXPImmediately();
            ForEachSeatWallet(seat, [&](Wallet& wallet) { wallet.Earn(reward, bank); });
        }
    }
    if (Net()->TryGetPlayerState(seat, victim->GetID())) {
        const bool handicap = defeat && !seat.is_bot && HasHandicap(seat.player_id);
        if (handicap || Net()->ModeSetting(defeat ? "progression_loss_death" : "progression_loss_damage")) {
            // Use the opponent's threat for combat and the victim's own bounty
            // for unattributed damage, including a fall with no last attacker.
            const double bounty = hostile ? Bounty(attacker, Observe(attacker).equipment) : Bounty(victim, spawn.equipment);
            const double amount = bounty * (defeat ? Rule("death_loss_factor") : Rule("damage_loss_factor") * damage);
            ForEachSeatWallet(seat, [&](Wallet& wallet) {
                if (handicap) wallet.Earn(amount, true);
                else wallet.Lose(amount, defeat);
            });
        }
    }
    dirty = true;
}
struct Offer {
    std::string id, label, description;
    double cost = 0;
    int rarity = 0;
    ID recipient = invalid;
    int armor_material = 0;
    int full_cost = 0;
    int heal_quote = 0;
    int next_race = -1;
};
std::string TierKey(const std::string& id, int tier) {
    return id + ":" + std::to_string(tier);
}
// The tier belongs to the purchase even when the same shop card advances
// to another material, race or level of training.
std::string RewardKey(const Offer& offer) {
    if (offer.armor_material || offer.next_race >= 0 || offer.id == "ai") return TierKey(offer.id, offer.rarity);
    return offer.id;
}
int WeaponPrice(const WeaponCategories::Category& weapon) {
    return static_cast<int>(Rule(std::string("weapon_") + weapon.price_family));
}
// An inventory action names the item as well as its socket, so a stale click
// cannot sell a replacement after a throw, pickup, or a second sale.
struct CarriedWeapon {
    ItemObject* item = nullptr;
    const WeaponCategories::Category* catalog = nullptr;
    const WeaponCategories::Skin* skin = nullptr;
    std::string key;
};
CarriedWeapon Carried(const PlayerState& player, int slot) {
    auto* actor = Actor(player.object_id);
    if (!actor || !actor->as_context || !actor->ASHasVar("weapon_slots")) return {};
    const int id = actor->ASGetArrayIntVar("weapon_slots", slot);
    SceneGraph* scene = Engine::Instance()->GetSceneGraph();
    if (id < 0 || !scene || !scene->DoesObjectWithIdExist(id)) return {};
    auto* object = scene->GetObjectFromID(id);
    if (!object || !object->enabled_ || object->GetType() != _item_object) return {};
    auto* item = static_cast<ItemObject*>(object);
    CarriedWeapon result;
    result.item = item;
    result.catalog = WeaponCategories::ForItem(item->obj_file, &result.skin);
    if (result.catalog)
        result.key = "sell_" + std::to_string(slot) + "_" + std::to_string(Net()->GetOriginalID(id)) + "_" + result.catalog->id;
    return result;
}
bool Sell(const PlayerState& buyer, const PlayerState& subject, const std::string& key) {
    for (int slot = 0; slot < 4; ++slot) {
        const auto held = Carried(subject, slot);
        if (!held.catalog || held.key != key || held.item->HeldByWhom() != subject.object_id) continue;
        auto* actor = Actor(subject.object_id);
        const int id = held.item->GetID();
        // Returning shop-issued gear relinquishes its unlock. Selling a pickup
        // with the same model leaves independently purchased gear intact.
        auto* params = held.item->GetScriptParams();
        if (params->HasParam("Progression Issued Weapon")) {
            ID owner = invalid;
            if (params->HasParam("Progression Issued To Slot"))
                owner = static_cast<ID>(params->ASGetInt("Progression Issued To Slot"));
            else {
                PlayerState seat;
                if (Net()->TryGetPlayerState(seat,params->ASGetInt("Progression Issued Weapon"))) owner = seat.player_id;
            }
            auto issued = run.players.find(owner);
            if (issued != run.players.end()) {
                auto& account = issued->second;
                account.weapons.erase(held.catalog->id);
                if (account.build.offhand_weapon == held.catalog->id) account.build.offhand_weapon.clear();
                if (account.build.main_weapon == held.catalog->id) account.build.main_weapon.clear();
            }
        } else if (!held.item->exclude_from_save) {
            // Authored map items return with their original IDs on a cold load.
            run.sold_authored[Net()->GetOriginalID(id)] = WeaponCategories::OldItemPath(held.skin->id) == held.item->obj_file
                ? held.skin->id : held.item->obj_file;
        }
        actor->NetworkDetachItem(id);
        actor->Execute("NotifyItemDetach(" + std::to_string(id) + ");");
        Net()->Send<OnlineMessages::AttachToMessage>(actor->GetID(), id, 0, false, false, nullptr, vec3(1), 0, true);
        Engine::Instance()->GetSceneGraph()->map_editor->DeleteID(id);
        Balance(buyer).Earn(WeaponPrice(*held.catalog), true);
        return true;
    }
    return false;
}
struct ShopStat {
    const char* key;
    const char* label;
    RankGains gains;
    const char* prefix;
    const char* benefit;
    int StatRanks::*rank;
};
constexpr ShopStat shop_stats[] = {
    {"health", "Durability", kDurabilityGain, "+", "% of base durability", &StatRanks::health},
    {"bone_damage", "Bone damage", kDamageGain, "+", "% to blunt impacts, throws and chokes", &StatRanks::bone_damage},
    {"flesh_damage", "Flesh damage", kDamageGain, "+", "% to cuts and bleeding", &StatRanks::flesh_damage},
    {"speed", "Movement speed", kSpeedGain, "+", "% of base movement speed", &StatRanks::speed},
    {"fall_resistance", "Fall resistance", kDamageGain, "Hard landings deal ", "% less damage", &StatRanks::fall_resistance},
    {"stealth_passive", "Passive stealth", kStealthGain, "Standing sight and hearing ranges shrink by ", "% of base range", &StatRanks::stealth_passive},
    {"stealth_active", "Active stealth", kStealthGain, "Crouched sight and hearing ranges shrink by ", "% of base range", &StatRanks::stealth_active},
    {"luck", "Luck", kLuckGain, "+", " percentage points to free choice chance, with better odds of valuable upgrades", &StatRanks::luck},
};
const ShopStat* Stat(const std::string& key) {
    for (const auto& stat : shop_stats)
        if (key == stat.key) return &stat;
    return nullptr;
}
bool CanTarget(const PlayerState& buyer, const PlayerState* subject) {
    return subject && ShopTeam(buyer) == ShopTeam(*subject) && (subject->is_bot || buyer.player_id == subject->player_id);
}
std::vector<ID> Rotation(int team, bool include_bots = false) {
    auto list = Members(team, include_bots);
    const auto start = std::upper_bound(list.begin(), list.end(), run.teams[team].last_recipient);
    std::rotate(list.begin(), start, list.end());
    return list;
}
std::vector<Offer> Catalog(const PlayerState& buyer, const PlayerState& target, bool free_choice = false) {
    std::vector<Offer> offers;
    if (run.restrict_upgrades && run.map_upgrades.empty()) return offers;
    int team_id = ShopTeam(buyer);
    auto& team = run.teams[team_id];
    const bool shared = Shared();
    Build build = GetBuild(target.player_id);
    std::vector<ID> choices = shared && !target.is_bot ? Rotation(team_id) : std::vector<ID>{target.player_id};
    auto offer = [&](std::string id, std::string label, std::string desc, double cost, int rarity, ID recipient) {
        offers.push_back({id, label, desc, cost, rarity, recipient});
    };
    if (!target.is_bot) {
        for (const auto& stat : shop_stats) {
            if (stat.rank == &StatRanks::luck && !FreeBuffs()) continue;
            const int rank = build.*stat.rank;
            if (rank >= kMaxRank) continue;
            std::ostringstream detail;
            const auto benefit = stat.rank == &StatRanks::fall_resistance ? 1.0 - FallDamageMultiplier(rank + 1) : stat.gains.Next(rank);
            detail << stat.prefix << std::round(benefit * 10000) / 100 << stat.benefit;
            if (shared) detail << " for " << target.playername << ". Each human teammate gains a rank";
            detail << ".";
            offer(stat.key, stat.label, detail.str(), UpgradePrice(rank), 0, target.player_id);
        }
    }
    // Prefer the first injured teammate in rotation. A healthy player can
    // still see the round's price ceiling in the paid shop.
    const unsigned rounds = free_choice ? 0 : run.completed_rounds;
    const int full_cost = static_cast<int>(Bound(std::round(Rule("restore_base") +
                                                            Rule("restore_per_round") * rounds), 1.0, 50000000.0));
    const PlayerState* heal_recipient = nullptr;
    int heal_cost = 0;
    for (ID id : choices) {
        if ((free_choice && id != target.player_id) || pending_restores.count(id)) continue;
        const auto* seat = Seat(id);
        auto* character = seat ? Actor(seat->object_id) : nullptr;
        if (!character) continue;
        const auto state = character->GetHealthRestoreState();
        if (!state.available) continue;
        if (state.Injured()) {
            heal_recipient = seat;
            heal_cost = std::max(1, static_cast<int>(std::lround(full_cost * static_cast<double>(state.damage))));
            break;
        }
        if (!free_choice && !heal_recipient) heal_recipient = seat;
    }
    if (heal_recipient) {
        // Free draws use the item's base value for weighting. Only purchases are priced by injury.
        offer("restore_health", "Restore health", "Heal all injuries and stop bleeding for " + heal_recipient->playername + ".",
              free_choice ? full_cost : heal_cost, 0, heal_recipient->player_id);
        if (!free_choice) {
            auto& heal = offers.back();
            heal.full_cost = full_cost;
            heal.heal_quote = heal_cost;
            heal.cost = std::min<double>(heal_cost, std::max(0.0, Spendable(buyer)));
            if (heal.cost < heal_cost)
                heal.description = "Spend your remaining XP to heal " + heal_recipient->playername +
                                   " for " + std::to_string(static_cast<int>(std::round(100.0 * heal.cost / heal_cost))) +
                                   "% of their injuries.";
        }
    }
    // Rotation breaks ties within the lowest remaining human tier. A targeted
    // companion has its own purchase path even when the party shares a wallet.
    int lowest = 4;
    ID race_recipient = invalid;
    for (ID id : choices) {
        const int race = HighestRace(*Seat(id));
        if (race >= 0 && race < lowest) {
            lowest = race;
            race_recipient = id;
        }
    }
    if (race_recipient != invalid) {
        const auto& next = races[lowest + 1];
        offer("race_upgrade", std::string(races[lowest].name) + " to " + next.name,
              "Upgrade " + Seat(race_recipient)->playername + " to " + next.name + ".",
              std::round(Rule("recruit_base") * next.recruit_price_multiplier), lowest + 1, race_recipient);
        offers.back().next_race = lowest + 1;
    }
    const auto armor_choices = shared ? Rotation(team_id, true) : std::vector<ID>{target.player_id};
    for (unsigned i = 0; i < 9; ++i) {
        int shared_material = ArmorProtection::Grey + 1;
        if (shared) for (ID id : armor_choices) {
            const int next = ArmorProtection::NextShopMaterial(i, run.players[id].build.armor[i]);
            if (next) shared_material = std::min(shared_material, next);
        }
        for (ID id : armor_choices) {
            const int material = ArmorProtection::NextShopMaterial(i, run.players[id].build.armor[i]);
            if (!material || (shared && material != shared_material)) continue;
            offer("armor_" + std::to_string(i), std::string(ArmorProtection::MaterialName(material)) + " / " + regions[i],
                  std::string(ArmorProtection::MaterialName(material)) + " for " + Seat(id)->playername,
                  ArmorPrice(i, material), material, id);
            offers.back().armor_material = material;
            break;
        }
    }
    for (const auto& weapon : WeaponCategories::Catalog()) {
        for (ID id : choices) {
            if (run.players[id].weapons.count(weapon.id)) continue;
            offer(weapon.id, weapon.label, "Unlock for " + Seat(id)->playername,
                  WeaponPrice(weapon), weapon.rarity, id);
            break;
        }
    }
    if (target.is_bot) {
        static const char* const prices[] = {"ai_hardcore", "ai_expert", "ai_nightmare"};
        if (build.ai < 3) offer("ai", training_names[build.ai], build.ai == 2 ? "Earns XP and trains its own stats between fights." : "Train this ally.", static_cast<int>(Rule(prices[build.ai])), build.ai + 2, target.player_id);
    } else if (!choices.empty() && Net()->HasSpaceForLobbySlot()) {
        offer("recruit","Recruit an ally","A new ally with Casual AI.", RecruitPrice(team.recruits, Rule("recruit_base"), Rule("recruit_growth")), 4, choices.front());
    }
    if (run.restrict_upgrades) {
        offers.erase(std::remove_if(offers.begin(), offers.end(), [](const Offer& candidate) {
            return !run.map_upgrades.count(RewardKey(candidate));
        }), offers.end());
    }
    return offers;
}
void CommitRestore(ID recipient, const PendingRestore& restore) {
    if (restore.hand) {
        auto& hands = run.players[restore.buyer].reward_hands;
        hands.erase(hands.begin(), hands.upper_bound(restore.hand));
    } else {
        Wallet& wallet = restore.team_wallet ? run.teams[restore.team_wallet].wallet : run.players[restore.buyer].wallet;
        wallet.Earn(-restore.cost, true);
        if (restore.rotate) run.teams[restore.team_wallet].last_recipient = recipient;
    }
}

bool GrantRestore(const PlayerState& buyer, const PlayerState& target, const Offer& offer, unsigned hand) {
    const auto* seat = Seat(offer.recipient);
    auto* character = seat ? Actor(seat->object_id) : nullptr;
    if (!character) return false;
    const PendingRestore receipt = {buyer.player_id, Shared() ? ShopTeam(buyer) : 0, seat->object_id,
                                    hand ? 0 : offer.cost, hand, !character->net_restore_phase, Shared() && !target.is_bot};
    const float fraction = hand ? 1.0f : static_cast<float>(offer.cost / offer.heal_quote);
    if (!Net()->ProgressionRestoreHealth(offer.recipient, fraction)) return false;
    if (character->net_restore_pending) {
        pending_restores.emplace(offer.recipient, receipt);
    } else if (character->net_restore_succeeded) {
        CommitRestore(offer.recipient, receipt);
    } else return false;
    return true;
}

void FinishRemoteRestores() {
    for (auto it = pending_restores.begin(); it != pending_restores.end();) {
        const PendingRestore& restore = it->second;
        const auto* seat = Seat(it->first);
        auto* character = seat && seat->object_id == restore.object && (seat->is_bot || seat->connected) ? Actor(restore.object) : nullptr;
        if (character && character->net_restore_pending) {
            ++it;
            continue;
        }
        if (character && character->net_restore_phase == restore.phase && character->net_restore_succeeded) {
            CommitRestore(it->first, restore);
        }
        it = pending_restores.erase(it);
        dirty = true;
    }
}

bool GrantOffer(const PlayerState& buyer, const PlayerState& target, const Offer& offer) {
    const auto& key = offer.id;
    const int team_id = ShopTeam(buyer);
    auto& team = run.teams[team_id];
    auto& account = run.players[offer.recipient];
    const auto* stat = Stat(key);
    if (stat || key == "damage") {
        StatRanks& ranks = Shared() ? static_cast<StatRanks&>(team) : static_cast<StatRanks&>(account.build);
        if (stat) ++(ranks.*stat->rank);
        else {
            // A pending reward drawn by an earlier build promised both effects.
            ++ranks.bone_damage;
            ++ranks.flesh_damage;
        }
        stats_cache.clear();
        return true;
    }
    if (key == "race_upgrade") {
        if (account.race < 0) account.first_race = offer.next_race - 1;
        account.race = offer.next_race;
        account.equipped_race = -1;
    } else if (key == "recruit") {
        ID bot = Net()->AddLobbyBot("", Net()->LobbyHostPlayerID());
        if (bot == invalid) return false;
        // The recruit belongs to the recipient's lobby team, including when
        // mode-managed teams temporarily share the same campaign party.
        Net()->SetLobbyTeam(bot, Seat(offer.recipient)->team, Net()->LobbyHostPlayerID());
        auto& build = run.players[bot].build;
        build.owner = offer.recipient;
        build.recruited = build.trained = true;
        ++team.recruits;
    } else if (key == "ai") {
        ++account.build.ai;
        account.build.trained = true;
    } else if (const auto* weapon = WeaponCategories::Find(key)) {
        account.weapons.insert(weapon->id);
        (weapon->offhand ? account.build.offhand_weapon : account.build.main_weapon) = weapon->id;
    } else {
        const unsigned region = static_cast<unsigned>(key.back() - '0'); // key came from the catalog
        account.build.armor[region] = offer.armor_material;
    }
    if (Shared() && (!target.is_bot || offer.armor_material)) team.last_recipient = offer.recipient;
    stats_cache.clear();
    return true;
}
bool Buy(const PlayerState& buyer, const PlayerState& target, const std::string& key) {
    const auto offers = Catalog(buyer, target);
    const auto it = std::find_if(offers.begin(), offers.end(), [&](const Offer& offer) { return offer.id == key; });
    if (it == offers.end() || Spendable(buyer) < it->cost) return false;
    if (key == "restore_health") return it->cost > 0 && GrantRestore(buyer, target, *it, 0);
    if (!GrantOffer(buyer, target, *it)) return false;
    Balance(buyer).held -= it->cost;
    return true;
}

std::vector<Offer> RewardPool(const PlayerState& buyer) {
    auto pool = Catalog(buyer, buyer, true);
    for (ID member : Members(ShopTeam(buyer), true)) {
        const auto& ally = *Seat(member);
        if (!ally.is_bot || GetBuild(member).ai >= 3) continue;
        const auto offers = Catalog(buyer, ally);
        const auto train = std::find_if(offers.begin(), offers.end(), [](const Offer& offer) { return offer.id == "ai"; });
        if (train != offers.end()) {
            pool.push_back(*train);
            pool.back().description = ally.playername + " / " + train->description;
            break;
        }
    }
    return pool;
}

void RollReward(const PlayerState& buyer) {
    static std::mt19937 random(std::random_device{}());
    const int luck = GetBuild(buyer.player_id).luck;
    if (!std::bernoulli_distribution(FreeRewardChance(luck))(random)) return;
    const double budget = FreeRewardBudget(luck, Rule("recruit_base"));
    std::vector<std::pair<double, std::string>> order;
    for (const auto& offer : RewardPool(buyer)) {
        const double weight = FreeRewardWeight(offer.cost, budget);
        // Each candidate races once at its own rate: the four smallest draws
        // are a weighted sample without replacement. The rest serve as backups.
        order.emplace_back(std::exponential_distribution<double>(weight)(random), RewardKey(offer));
    }
    std::sort(order.begin(), order.end());
    if (!order.empty()) {
        auto& hand = run.players[buyer.player_id].reward_hands[run.reward_generation];
        for (const auto& entry : order) hand.push_back(entry.second);
    }
}
void RollCheckpointRewards(const std::set<int>* participants = nullptr) {
    if (!FreeBuffs() || run.reward_generation == UINT32_MAX) return;
    ++run.reward_generation;
    for (const auto& entry : Net()->online_session->player_states) {
        const auto& p = entry.second;
        if (p.connected && !p.is_bot && (!participants || participants->count(p.object_id))) RollReward(p);
    }
    dirty = true;
}
std::vector<Offer> RewardCards(const PlayerState& buyer, unsigned& generation) {
    generation = 0;
    const auto& hands = run.players[buyer.player_id].reward_hands;
    std::vector<Offer> cards;
    if (!FreeBuffs() || hands.empty()) return cards;
    const auto pool = RewardPool(buyer);
    auto find = [&](const std::string& wanted) {
        return std::find_if(pool.begin(), pool.end(), [&](const Offer& o) { return RewardKey(o) == wanted; });
    };
    for (const auto& hand : hands) {
        for (const auto& key : hand.second) {
            if (key == "damage") {
                const auto bone = find("bone_damage"), flesh = find("flesh_damage");
                if (bone != pool.end() && flesh != pool.end()) {
                    cards.push_back({"damage", "Bone and flesh damage", "Gain one rank in both damage stats.", 0, 0, buyer.player_id});
                } else if (bone != pool.end()) cards.push_back(*bone);
                else if (flesh != pool.end()) cards.push_back(*flesh);
            } else {
                const auto item = find(key);
                if (item != pool.end()) cards.push_back(*item);
            }
            if (cards.size() == 4) break;
        }
        if (!cards.empty()) {
            generation = hand.first;
            break;
        }
    }
    return cards;
}
bool ClaimReward(const PlayerState& buyer, const Offer& offer, unsigned generation) {
    const auto pending = pending_restores.find(buyer.player_id);
    if (pending != pending_restores.end() && pending->second.hand) return false;
    if (offer.id == "restore_health") return GrantRestore(buyer, buyer, offer, generation);
    const auto* subject = offer.id == "ai" ? Seat(offer.recipient) : &buyer;
    if (!subject || !GrantOffer(buyer, *subject, offer)) return false;
    if (const auto* weapon = WeaponCategories::Find(offer.id)) Net()->ProgressionWeaponAwarded(offer.recipient, weapon->offhand);
    auto& hands = run.players[buyer.player_id].reward_hands;
    hands.erase(hands.begin(), hands.upper_bound(generation));
    return true;
}
bool HasSurvivalChoice(const PlayerState& seat) {
    if (!seat.connected || seat.is_bot || !Net()->IsSurvivalMode() || !FreeBuffs()) return false;
    const auto pending = pending_restores.find(seat.player_id);
    if (pending != pending_restores.end() && pending->second.hand) return true;
    const auto account = run.players.find(seat.player_id);
    if (account == run.players.end() || account->second.reward_hands.empty()) return false;
    unsigned hand = 0;
    return !RewardCards(seat, hand).empty();
}
void TrainBots() {
    for (auto& entry : run.players) {
        auto& account = entry.second;
        auto& build = account.build;
        if (build.ai != 3) continue;
        for (;;) {
            int* lowest = nullptr;
            for (const auto& stat : shop_stats) {
                if (stat.rank == &StatRanks::luck || (run.restrict_upgrades && !run.map_upgrades.count(stat.key))) continue;
                int& rank = build.*stat.rank;
                if (!lowest || rank < *lowest) lowest = &rank;
            }
            if (!lowest || *lowest >= kMaxRank || account.wallet.held < UpgradePrice(*lowest)) break;
            account.wallet.held -= UpgradePrice(*lowest);
            ++*lowest;
        }
    }
}
int SecondsLeft() {
    return run.deadline_ms <= 0 ? -1 : int(std::max(0.0, std::ceil((run.deadline_ms - OnlineClockMs()) / 1000.0)));
}
}

std::vector<UpgradeDefinition> UpgradeCatalog() {
    std::vector<UpgradeDefinition> result;
    for (const auto& stat : shop_stats)
        result.push_back({stat.key, stat.label, "", "", 0});
    result.push_back({"restore_health", "Restore health", "", "", 1});
    const std::string thumbs = "Data/UI/spawner/thumbs/";
    const char* const dog_armor[] = {"dog_armor_breastplate", "dog_armor_backplate", "dog_armor_helmet", "dog_armor_lower_arm",
                                     "dog_armor_lower_arm", "dog_armor_shoulder_b", "dog_armor_shoulder_b", "dog_armor_hip_plate", "dog_armor_hip_plate"};
    for (unsigned region = 0; region < 9; ++region) {
        const auto slot = "armor_" + std::to_string(region);
        std::string prerequisite;
        for (int tier = ArmorProtection::NextShopMaterial(region, 0); tier; tier = ArmorProtection::NextShopMaterial(region, tier)) {
            auto id = TierKey(slot, tier);
            std::string artwork;
            if (tier == ArmorProtection::Wood) artwork = thumbs + "O2/large_wood_plate.png";
            if (tier == ArmorProtection::Dog) artwork = thumbs + "Object/" + dog_armor[region] + ".png";
            result.push_back({id, std::string(ArmorProtection::MaterialName(tier)) + " / " + regions[region], artwork, prerequisite, tier});
            prerequisite = id;
        }
    }
    for (const auto& weapon : WeaponCategories::Catalog())
        result.push_back({weapon.id, weapon.label, weapon.skins.front().artwork, "", std::max(1, weapon.rarity)});
    const char* const race_art[] = {"turner", "male_cat_actor", "dog", "male_wolf_actor"};
    for (int tier = 1; tier < 5; ++tier) {
        std::string label = std::string(races[tier - 1].name) + " to " + races[tier].name;
        result.push_back({TierKey("race_upgrade", tier), label, thumbs + "Character/" + race_art[tier - 1] + ".png",
                          tier == 1 ? "" : TierKey("race_upgrade", tier - 1), tier});
    }
    result.push_back({"recruit", "Recruit an ally", thumbs + "Character/turner.png", "", 4});
    for (int tier = 2; tier <= 4; ++tier)
        result.push_back({TierKey("ai", tier), training_names[tier - 2], thumbs + "Character/rabbitguard.png",
                          tier == 2 ? "" : TierKey("ai", tier - 1), tier});
    return result;
}
void CaptureMapUpgrades() {
    if (!Net()->IsHosting() || Net()->LobbyWaiting()) return;
    const auto owned = LocalProfiles::UpgradePool(run.restrict_upgrades);
    run.map_upgrades.clear();
    // Keep inactive mod entries in the profile so they work again on reinstall.
    // Only installed upgrade definitions belong to this map's shared catalog.
    if (!owned.empty()) {
        for (const auto& upgrade : UpgradeCatalog())
            if (owned.count(upgrade.id)) run.map_upgrades.insert(upgrade.id);
    }
    dirty = true;
}
bool Enabled() {
    return Net()->IsActive() && !Net()->LobbyBuild() && ModLoading::Instance().HasSessionFeature("progression") && Net()->ModeSetting("progression") != 0;
}
bool HasHandicap(ID id) {
    const auto account = run.players.find(id);
    return account != run.players.end() && account->second.handicap;
}
bool CanSetHandicap(ID actor, ID id) {
    if (!Net()->LobbyWaiting() || Net()->LobbyMigrationBusy() || !Enabled()) return false;
    const auto* target = Seat(id);
    const auto* slot = Net()->GetLobbySlot(id);
    if (!slot || slot->is_bot) return false;
    if (Net()->IsLobbyHost(actor)) {
        // Hosts can prepare a human placeholder before a controller or peer joins.
        return true;
    }
    const auto* owner = Seat(actor);
    return target && owner && owner->peer_id == target->peer_id &&
           Net()->IsLobbyLeader(actor) && Net()->ModeSetting("progression_local_handicaps");
}
bool SetHandicap(ID actor, ID id, bool enabled) {
    if (!CanSetHandicap(actor, id)) return false;
    if (Net()->IsClient()) {
        Json::Value request;
        request["action"] = "progress_handicap"; request["actor"] = actor;
        request["id"] = id; request["enabled"] = enabled;
        Net()->Send<OnlineMessages::LobbyControl>(Json::FastWriter().write(request));
    } else if (HasHandicap(id) != enabled) {
        run.players[id].handicap = enabled;
        Publish();
    }
    return true;
}
bool ShopOpen() { return run.open && Enabled(); }
bool ApplyRace(ID id, bool bot, MovementObject& avatar) {
    if (!avatar.as_context || !avatar.rigged_object()) return false;
    int race = StartingRace(bot);
    const auto it = run.players.find(id);
    if (Enabled() && it != run.players.end() && it->second.race >= 0) {
        auto& account = it->second;
        if (account.first_race < 0 && Net()->IsHosting()) {
            account.first_race = LegacyFirstRace(bot, account.race, avatar);
            dirty = true;
        }
        race = account.equipped_race >= 0 ? account.equipped_race : account.race;
    }
    if (race < 0) return Enabled() && Net()->IsCampaignPlayMode() && NaturalRace(avatar) >= 0;
    // The replicated Species parameter can arrive before the new skeleton.
    // Keep authored skins when the equipped body and its gameplay species agree.
    const auto& desired = races[race];
    if (avatar.rig_character_path != desired.character &&
        (NaturalRace(avatar) != race || avatar.character_script_getter.GetTag("species") != desired.species)) {
        std::string authored;
        const bool original = !bot && Net()->IsCampaignPlayMode() && it != run.players.end() &&
                              it->second.first_race == race && AuthoredRace(avatar, &authored) == race;
        avatar.ApplyNetworkCharacter(original ? authored : desired.character);
    }
    return true;
}
int SurvivalShopSeconds() {
    const int index = Net()->ModeSetting("progression_timer");
    if (index == 0) return -1;
    return index > 0 && index < 6 ? timers[index] : 0;
}
bool LoopChosen() { return run.final && run.loop && Enabled(); }
void CloseSurvivalUpgrades() {
    if (!Net()->IsHosting() || !Net()->IsSurvivalMode()) return;
    FinishRemoteRestores();
    for (auto& account : run.players) account.second.reward_hands.clear();
    run.open = false;
    run.deadline_ms = 0;
    run.ready.clear();
    Publish();
}
Build GetBuild(ID id) {
    const auto found = run.players.find(id);
    Build b = found == run.players.end() ? Build{} : found->second.build;
    const auto* player = Seat(id);
    if (player && !player->is_bot) {
        const auto team = run.teams.find(ShopTeam(*player));
        if (team != run.teams.end()) {
            for (const auto& stat : shop_stats) b.*stat.rank += team->second.*stat.rank;
            b.luck = std::min(b.luck, kMaxRank);
        }
    }
    return b;
}
float AttackMultiplier(int attacker, bool flesh) {
    if (attacker < 0 || !Enabled()) return 1.0f;
    PlayerState seat;
    if (!Net()->TryGetPlayerState(seat, attacker)) return 1.0f;
    const auto build = GetBuild(seat.player_id);
    return DamageMultiplier(flesh ? build.flesh_damage : build.bone_damage);
}
void SetSavedProfileRun(const std::string& run_id) {
    run.profile_run_id = run_id;
}
void NewSession(bool keep_slot_handicaps) {
    Run fresh;
    fresh.profile_run_id = LocalProfiles::NewID();
    pending_restores.clear();
    if (keep_slot_handicaps) {
        for (const auto& entry : run.players)
            if (entry.second.handicap) fresh.players[entry.first].handicap = true;
    }
    run = std::move(fresh);
    spawns.clear();
    last_action.clear();
    next_action = 0;
    dirty = false;
    next_sample = next_send = 0;
    stats_cache.clear();
}
void ForgetPlayer(ID id) {
    if (!Net()->IsHosting() || Net()->LobbyMigrationBusy()) return;
    for (auto it = pending_restores.begin(); it != pending_restores.end();) {
        if (it->first == id || it->second.buyer == id) it = pending_restores.erase(it);
        else ++it;
    }
    run.players.erase(id);
    run.xp_scales.erase(id);
    stats_cache.erase(id);
    run.ready.erase(id);
    last_action.erase(id);
    for (auto& team : run.teams)
        if (team.second.leader == id) team.second.leader = invalid;
    for (auto& player : run.players)
        if (player.second.build.owner == id) player.second.build.owner = invalid;
    dirty = true;
}
void ResetAttempt() {
    run.profile_event.clear();
    if (!Net()->IsHosting() || !Enabled()) return;
    if (Arena() && run.round_completed) {
        // The remote owner's last hit report can arrive during the winner camera.
        CommitPooledXP();
    } else {
        for (auto& p : run.players) p.second.wallet.pooled = 0;
        for (auto& t : run.teams) t.second.wallet.pooled = 0;
    }
    spawns.clear();
    run.round_completed = run.match_completed = false;
    ++run.attempt;
    dirty = true;
}
void NewWorld() {
    pending_restores.clear();
    spawns.clear();
    stats_cache.clear();
    run.xp_scales.clear();
    next_sample = 0;
    if (!Net()->IsHosting()) return;
    run.open = run.final = run.loop = false;
    run.checkpoints.clear();
    run.sold_authored.clear();
    restore_sales = false;
    run.survival_clears.clear();
    run.survival_battery = 0;
    run.deadline_ms = 0;
    run.ready.clear();
    ResetAttempt();
}
void Checkpoint(int checkpoint, int source) {
    if (!Enabled() || !Net()->IsHosting()) return;
    if (checkpoint != 0) {
        if (!Net()->IsCampaignPlayMode() || checkpoint < 0 || checkpoint > 4096 || source < -1) return;
        auto& last = run.checkpoints[source];
        if (checkpoint <= last) return;
        last = checkpoint;
        RollCheckpointRewards();
        if (Net()->ModeSetting("progression_gain_checkpoint")) {
            std::vector<const PlayerState*> recipients;
            for (const auto& row : Net()->online_session->player_states) {
                const auto& seat = row.second;
                if (seat.is_bot || seat.connected) recipients.push_back(&seat);
            }
            RewardRecipients(recipients, Rule("checkpoint_xp"), false);
        }
    }
    CommitPooledXP();
}
static void PayWin(const std::vector<int>& winners, bool match) {
    if (!Net()->ModeSetting(match ? "progression_gain_match" : "progression_gain_round")) return;
    std::set<int> winning_avatars(winners.begin(), winners.end());
    std::vector<const PlayerState*> recipients;
    for (const auto& row : Net()->online_session->player_states) {
        const auto& seat = row.second;
        if ((seat.is_bot || seat.connected) && winning_avatars.count(seat.object_id)) recipients.push_back(&seat);
    }
    RewardRecipients(recipients, Rule(match ? "match_win_xp" : "round_win_xp"), true);
}
static void RecordProfileClear(LocalProfiles::ClearKind kind, const std::string& event, const std::vector<int>* winners) {
    if (event == run.profile_event || run.profile_completion == UINT32_MAX) return;
    if (run.profile_run_id.empty()) run.profile_run_id = LocalProfiles::NewID();
    run.profile_event = event;
    LocalProfiles::Award(kind, run.profile_run_id, ++run.profile_completion, winners);
    dirty = true;
}
void ArenaWin(const std::vector<int>& winners, bool match, const std::vector<int>* participants) {
    if (Net()->IsHosting() && Arena() && match && !winners.empty())
        RecordProfileClear(LocalProfiles::ClearKind::ArenaMatch, "arena_match", &winners);
    if (!Enabled() || !Net()->IsHosting() || !Arena()) return;
    bool& completed = match ? run.match_completed : run.round_completed;
    if (completed) return;
    completed = true;
    dirty = true;
    if (!match) {
        run.CompleteRound();
        CommitPooledXP();
        std::set<int> roster;
        if (participants) roster.insert(participants->begin(), participants->end());
        RollCheckpointRewards(participants ? &roster : nullptr);
    }
    PayWin(winners, match);
}
void SurvivalClear(int battery, int wave, const std::vector<int>& winners) {
    if (Net()->IsHosting() && Net()->IsSurvivalMode() && battery > 0 && wave == 0 && !winners.empty())
        RecordProfileClear(LocalProfiles::ClearKind::SurvivalBattery, "battery_" + std::to_string(battery), &winners);
    if (!Enabled() || !Net()->IsHosting() || !Net()->IsSurvivalMode() || winners.empty() || battery < 1 ||
        battery < run.survival_battery || wave < 0 || wave > 8192) return;
    if (battery > run.survival_battery) {
        run.survival_clears.clear();
        run.survival_battery = battery;
    }
    if (run.survival_clears.count({0, 0})) return;
    const bool match = wave == 0;
    // Each team reports independently, and waves can finish out of order.
    const bool new_wave = !match && std::none_of(run.survival_clears.begin(), run.survival_clears.end(),
                                                 [wave](const std::pair<int, int>& clear) { return clear.second == wave; });
    const std::set<int> winning(winners.begin(), winners.end());
    std::map<int, std::vector<const PlayerState*>> teams;
    for (const auto& entry : Net()->online_session->player_states) {
        const auto& seat = entry.second;
        if ((seat.connected || seat.is_bot) && winning.count(seat.object_id)) teams[ShopTeam(seat)].push_back(&seat);
    }
    std::vector<int> paid;
    for (const auto& team : teams) {
        if (!run.survival_clears.insert({team.first, wave}).second) continue;
        if (Shared()) run.teams[team.first].wallet.Commit();
        for (const auto* seat : team.second) {
            if (Wallet* wallet = PersonalXPWallet(*seat)) wallet->Commit();
            paid.push_back(seat->object_id);
        }
    }
    if (paid.empty()) return;
    if (new_wave) run.CompleteRound();
    dirty = true;
    PayWin(paid, match);
    if (match) {
        run.survival_clears.insert({0, 0});
        const std::set<int> participants(paid.begin(), paid.end());
        RollCheckpointRewards(&participants);
    }
}

void CampaignComplete() {
    if (Net()->IsHosting() && !Net()->LobbyWaiting() && Net()->IsCampaignPlayMode())
        RecordProfileClear(LocalProfiles::ClearKind::CampaignLevel, "campaign_level", nullptr);
}
bool BeginShop(bool final) {
    if (!Enabled() || !Net()->IsHosting() || Net()->LobbyWaiting()) return false;
    if (run.open) return true;
    if (!Arena() && !Net()->IsSurvivalMode() && Net()->IsCampaignPlayMode()) run.CompleteRound();
    if (!Net()->IsSurvivalMode()) Checkpoint();
    if (SceneGraph* graph = Engine::Instance()->GetSceneGraph()) SampleXPScales(graph);
    if (PaidShop()) TrainBots();
    ++run.shop_generation;
    run.open = PaidShop() || final;
    if (!run.open && Net()->IsSurvivalMode())
        for (const auto& row : Net()->online_session->player_states)
            if (HasSurvivalChoice(row.second)) { run.open = true; break; }
    run.final = final;
    run.loop = false;
    run.ready.clear();
    last_action.clear();
    if (!run.open) {
        Publish();
        return false;
    }
    int humans = 0;
    std::set<int> teams;
    for (const auto& entry : Net()->online_session->player_states) {
        if (!entry.second.is_bot && entry.second.connected) {
            ++humans;
            if (!PaidShop() && !Net()->IsLobbyHost(entry.first)) run.ready.insert(entry.first);
            teams.insert(ShopTeam(entry.second));
        }
    }
    for (int team : teams) Leader(team);
    int index = Net()->ModeSetting("progression_timer");
    // Survival's director counts in gameplay time and closes the shop at deployment.
    int seconds = !Net()->IsSurvivalMode() && humans > 1 && index >= 0 && index < 6 ? timers[index] : 0;
    run.deadline_ms = seconds ? OnlineClockMs() + 1000.0 * seconds : 0;
    Publish();
    return true;
}
static void FinishShop() {
    if (Net()->IsSurvivalMode()) for (auto& account : run.players) account.second.reward_hands.clear();
    run.open = false;
    run.deadline_ms = 0;
    spawns.clear(); // Arena reuses the same object IDs for the next match.
    ++run.attempt;
    Publish();
}
void Update() {
    if (!Net()->IsHosting() || !Net()->online_session || Net()->LobbyWaiting() || !Enabled()) return;
    FinishRemoteRestores();
    if (restore_sales && Net()->host_started_level && Net()->SendingRemovePackages() && Engine::Instance()->IsLevelLoaded()) {
        SceneGraph* scene = Engine::Instance()->GetSceneGraph();
        for (const auto& sale : run.sold_authored) {
            const int id = Net()->GetObjectID(sale.first);
            const std::string expected = OnlineUtility::IsSafeAssetPath(sale.second) ? sale.second : WeaponCategories::OldItemPath(sale.second);
            auto* item = scene->DoesObjectWithIdExist(id) ? scene->GetObjectFromID(id) : nullptr;
            if (item && item->GetType() == _item_object && !item->exclude_from_save && item->obj_file == expected)
                scene->map_editor->DeleteID(id);
        }
        restore_sales = false;
    }
    const double now = OnlineClockMs();
    if (run.open) {
        bool waiting = false;
        const bool paid = PaidShop();
        const bool expired = !Net()->IsSurvivalMode() && run.deadline_ms > 0 && now >= run.deadline_ms;
        for (const auto& row : Net()->online_session->player_states) {
            const auto& seat = row.second;
            if (seat.is_bot || !seat.connected) continue;
            if (HasSurvivalChoice(seat) || (!run.ready.count(row.first) && (paid || (run.final && Net()->IsLobbyHost(row.first))))) waiting = true;
        }
        if (!waiting || expired) FinishShop();
    } else if (now >= next_sample && Net()->host_started_level && Engine::Instance()->IsLevelLoaded()) {
        next_sample = now + 350;
        SceneGraph* graph = Engine::Instance()->GetSceneGraph();
        if (graph) {
            std::set<int> present;
            for (auto* object : graph->movement_objects_) {
                auto* actor = static_cast<MovementObject*>(object);
                if (actor->enabled_ && actor->as_context) {
                    Observe(actor);
                    present.insert(actor->GetID());
                }
            }
            for (auto it = spawns.begin(); it != spawns.end();) {
                if (!present.count(it->first)) it = spawns.erase(it); else ++it;
            }
            SampleXPScales(graph);
        }
    }
    if (dirty && now >= next_send) Publish();
}
void ReportHit(int victim, int attacker, float damage, bool defeat) {
    if (!Enabled() || run.open || !std::isfinite(damage) || !Net()->IsLocalAvatar(victim)) return;
    if (!defeat && (damage <= 0 || (!Net()->ModeSetting("progression_gain_damage") &&
                                    !Net()->ModeSetting("progression_loss_damage")))) return;
    if (Net()->IsHosting()) {
        ApplyHit(Actor(victim), Actor(attacker), damage, defeat);
    } else if (Net()->IsClient()) {
        Json::Value message;
        message["action"] = "progress_hit";
        message["attempt"] = run.attempt;
        message["victim"] = Net()->GetOriginalID(victim);
        message["attacker"] = Net()->GetOriginalID(attacker);
        message["damage"] = damage;
        message["defeat"] = defeat;
        Net()->Send<OnlineMessages::LobbyControl>(Json::FastWriter().write(message));
    }
}
void ReportRecovery(int victim) {
    PlayerState seat;
    if (!Enabled() || run.open || !Net()->IsLocalAvatar(victim) || !Net()->TryGetPlayerState(seat, victim)) return;
    if (Net()->IsHosting()) {
        spawns.erase(victim);
    } else if (Net()->IsClient()) {
        Json::Value message;
        message["action"] = "progress_recover";
        message["attempt"] = run.attempt;
        message["victim"] = Net()->GetOriginalID(victim);
        Net()->Send<OnlineMessages::LobbyControl>(Json::FastWriter().write(message));
    }
}
bool ReceiveEvent(uint8_t peer, const Json::Value& m) {
    if (!Enabled() || !Net()->IsHosting() || Net()->LobbyWaiting() || run.open || !m["attempt"].isUInt() ||
        m["attempt"].asUInt() != run.attempt || !m["victim"].isInt()) return false;
    const int victim = Net()->GetObjectID(m["victim"].asInt());
    if (Net()->GetAvatarOwner(victim) != peer) return false;
    if (m["action"].asString() == "progress_recover") {
        PlayerState seat;
        if (!Net()->TryGetPlayerState(seat, victim)) return false;
        spawns.erase(victim);
        return true;
    }
    if (!m["attacker"].isInt() || !m["damage"].isNumeric() || !m["defeat"].isBool()) return false;
    const float damage = m["damage"].asFloat();
    if (!std::isfinite(damage) || damage < 0 || damage > 1.001f) return false;
    ApplyHit(Actor(victim), Actor(Net()->GetObjectID(m["attacker"].asInt())), damage, m["defeat"].asBool());
    return true;
}
Json::Value View(ID actor, ID target) {
    Json::Value v;
    const auto* buyer = Seat(actor);
    v["enabled"] = Enabled() && buyer && !Net()->LobbyWaiting();
    if (!v["enabled"].asBool()) return v;
    v["shop_open"] = run.open;
    v["shop_enabled"] = PaidShop();
    v["xp_enabled"] = true;
    if (run.open) {
        v["ready"] = run.ready.count(actor) != 0;
        v["can_force_finish"] = Net()->IsLobbyHost(actor);
        v["seconds_left"] = SecondsLeft();
        v["final"] = run.final;
        v["loop"] = run.loop;
        v["can_loop"] = run.final && Net()->IsLobbyHost(actor);
    }
    auto& w = Balance(*buyer);
    v["held"] = w.held;
    v["pooled"] = w.pooled;
    v["commit_amount"] = w.commit_amount;
    v["commit_serial"] = w.commit_serial;
    const auto scale = run.xp_scales.find(actor);
    if (scale != run.xp_scales.end()) v["xp_scale"] = scale->second;
    const auto* subject = run.open ? Seat(target) : buyer;
    if (!CanTarget(*buyer,subject)) subject = buyer;
    v["stats"] = PlayerStats(*subject);
    auto card_view = [&](const Offer& offer, bool free) {
        Json::Value card;
        if (offer.armor_material) card["armor_material"] = offer.armor_material;
        card["id"] = offer.id; card["label"] = offer.label; card["description"] = offer.description;
        card["rarity"] = offer.rarity; card["cost"] = offer.cost;
        if (offer.full_cost) {
            card["full_cost"] = offer.full_cost;
            card["heal_quote"] = offer.heal_quote;
            card["heal_fraction"] = offer.heal_quote ? offer.cost / offer.heal_quote : 0.0;
        }
        card["available"] = free || ((!offer.full_cost || offer.cost > 0) &&
                                     !run.ready.count(actor) && Spendable(*buyer) >= offer.cost);
        return card;
    };
    unsigned hand = 0;
    v["reward_cards"] = Json::Value(Json::arrayValue);
    for (const auto& offer : RewardCards(*buyer, hand)) v["reward_cards"].append(card_view(offer, true));
    v["reward_pending"] = hand != 0;
    v["reward_generation"] = hand;
    if (!run.open) return v;
    const int team = ShopTeam(*buyer);
    const ID leader = Leader(team);
    v["shared"] = Shared();
    v["team_leader"] = leader;
    v["can_buy"] = PaidShop() && (!Shared() || actor == leader);
    if (run.restrict_upgrades) v["map_unlock_count"] = static_cast<Json::UInt>(run.map_upgrades.size());
    v["targets"] = Json::Value(Json::arrayValue);
    v["teammates"] = Json::Value(Json::arrayValue);
    for (ID id : Members(team,true)) {
        const auto& p = *Seat(id);
        Json::Value row;
        row["id"] = id; row["name"] = p.playername; row["bot"] = p.is_bot;
        if (CanTarget(*buyer,&p)) v["targets"].append(row);
        if (!p.is_bot) v["teammates"].append(row);
    }
    const auto& account = run.players[subject->player_id];
    v["equip_target"] = subject->player_id;
    const int highest = HighestRace(*subject);
    const int first = account.first_race < 0 ? highest : account.first_race;
    const int equipped = account.equipped_race < 0 ? highest : account.equipped_race;
    v["races"] = Json::Value(Json::arrayValue);
    for (int tier = first; tier >= 0 && tier <= highest; ++tier) {
        Json::Value option;
        option["id"] = races[tier].species;
        option["name"] = races[tier].name;
        v["races"].append(option);
    }
    if (equipped >= 0) v["equipped_race"] = races[equipped].species;
    for (const auto& weapon : WeaponCategories::Catalog())
        v["owned_" + weapon.id] = account.weapons.count(weapon.id) != 0;
    v["main_category"] = account.build.main_weapon;
    v["offhand_category"] = account.build.offhand_weapon;
    v["dagger_variant"] = std::max(0, WeaponCategories::ToLegacy(account.build.offhand_weapon));
    v["big_weapon"] = std::max(0, WeaponCategories::ToLegacy(account.build.main_weapon));
    v["dagger"] = !account.build.offhand_weapon.empty();
    v["can_equip"] = subject == buyer || (subject->is_bot && v["can_buy"].asBool());
    static const char* const slots[] = {"Left hand", "Right hand", "Left belt", "Right belt"};
    v["inventory"] = Json::Value(Json::arrayValue);
    for (int slot = 0; slot < 4; ++slot) {
        const auto held = Carried(*subject, slot);
        Json::Value row;
        row["slot"] = slots[slot];
        row["item"] = held.item ? static_cast<int>(Net()->GetOriginalID(held.item->GetID())) : -1;
        if (held.catalog) {
            row["label"] = held.skin->label;
            row["price"] = WeaponPrice(*held.catalog);
            row["sell_key"] = held.key;
        } else if (held.item) {
            const auto& path = held.item->obj_file;
            row["label"] = path.substr(path.find_last_of('/') + 1);
        }
        v["inventory"].append(row);
    }
    v["cards"] = Json::Value(Json::arrayValue);
    if (v["can_buy"].asBool()) for (const auto& offer : Catalog(*buyer,*subject))
        v["cards"].append(card_view(offer, false));
    return v;
}
bool ReceiveAction(ID actor, const Json::Value& m) {
    const auto* buyer = Seat(actor);
    if (!Enabled() || Net()->LobbyWaiting() || !Net()->IsHosting() || !buyer || buyer->is_bot || !buyer->connected ||
        !m["generation"].isUInt() || !m["seq"].isUInt() ||
        !m["command"].isString() || !m["key"].isString() || !m["target"].isUInt()) return false;
    const auto action = m["command"].asString(), key = m["key"].asString();
    const ID target = m["target"].asUInt();
    if (action == "reward") {
        if (target != actor) return false;
        unsigned hand;
        const auto rewards = RewardCards(*buyer, hand);
        if (!hand || m["generation"].asUInt() != hand) return false;
        const auto offer = std::find_if(rewards.begin(), rewards.end(), [&](const Offer& o) { return o.id == key; });
        if (offer == rewards.end() || !ClaimReward(*buyer, *offer, hand)) return false;
        Publish();
        return true;
    }
    if (!run.open || m["generation"].asUInt() != run.shop_generation || m["seq"].asUInt() <= last_action[actor]) return false;
    last_action[actor] = m["seq"].asUInt();
    if (action == "force_finish") {
        if (!Net()->IsLobbyHost(actor)) return false;
        FinishRemoteRestores();
        FinishShop();
        return true;
    }
    const auto* subject = Seat(target);
    const int team_id = ShopTeam(*buyer);
    const bool may_buy = !Shared() || actor == Leader(team_id);
    if (action == "ready") {
        if (run.ready.count(actor)) run.ready.erase(actor); else run.ready.insert(actor);
    } else if (run.ready.count(actor)) return false;
    else if (action == "loop" && run.final && Net()->IsLobbyHost(actor) && (key == "yes" || key == "no")) run.loop = key == "yes";
    else if (!PaidShop()) return false;
    else if (action == "leader" && actor == Leader(team_id) && subject && !subject->is_bot && subject->connected && team_id == ShopTeam(*subject))
        run.teams[team_id].leader = target;
    else if (!CanTarget(*buyer,subject)) return false;
    else if (action == "buy" && may_buy) {
        if (!Buy(*buyer,*subject,key)) return false;
    } else if (action == "sell" && may_buy) {
        if (!Sell(*buyer, *subject, key)) return false;
    } else if (action == "equip_race" && (target == actor || (subject->is_bot && may_buy))) {
        auto& account = run.players[target];
        const int highest = HighestRace(*subject);
        const int first = account.first_race < 0 ? highest : account.first_race;
        int tier = first;
        while (tier >= 0 && tier <= highest && key != races[tier].species) ++tier;
        if (tier < 0 || tier > highest) return false;
        account.equipped_race = tier == highest ? -1 : tier;
    } else if (action == "equip" && (target == actor || (subject->is_bot && may_buy))) {
        auto& acc = run.players[target];
        if (key == "none") acc.build.main_weapon.clear();
        else {
            const auto* weapon = WeaponCategories::Find(key);
            if (!weapon || !acc.weapons.count(key)) return false;
            if (weapon->offhand && acc.build.offhand_weapon == key) acc.build.offhand_weapon.clear();
            else (weapon->offhand ? acc.build.offhand_weapon : acc.build.main_weapon) = key;
        }
    } else return false;
    Publish();
    return true;
}
bool Act(ID actor, ID target, const std::string& action, const std::string& key, unsigned expected_reward_generation) {
    const auto* owner = Seat(actor);
    if (!Enabled() || Net()->LobbyWaiting() || !owner || owner->peer_id != Net()->LocalPeerID() || owner->is_bot) return false;
    if (action == "force_finish" && !Net()->IsLobbyHost(actor)) return false;
    unsigned generation = run.shop_generation;
    if (action == "reward") {
        if (target != actor) return false;
        if (expected_reward_generation) generation = expected_reward_generation;
        else {
            RewardCards(*owner, generation);
            if (!generation) return false;
        }
    } else if (!run.open) return false;
    Json::Value m;
    m["action"] = "progress_action"; m["command"] = action; m["key"] = key;
    m["actor"] = actor; m["target"] = target; m["generation"] = generation;
    m["seq"] = ++next_action;
    if (Net()->IsHosting()) return ReceiveAction(actor,m);
    Net()->Send<OnlineMessages::LobbyControl>(Json::FastWriter().write(m));
    return true;
}
namespace {
Json::Value WriteWallet(const Wallet& w) {
    Json::Value j;
    j["held"] = w.held; j["pooled"] = w.pooled;
    j["serial"] = w.commit_serial; j["amount"] = w.commit_amount;
    return j;
}
bool ReadWallet(const Json::Value& j, Wallet& w) {
    if (!j.isObject() || !j["held"].isNumeric() || !j["pooled"].isNumeric() || !j["amount"].isNumeric() || !j["serial"].isUInt()) return false;
    auto balance = [](double v) { return std::isfinite(v) && v >= 0 && v <= Wallet::kMaxExactXP; };
    w.held = j["held"].asDouble(); w.pooled = j["pooled"].asDouble(); w.commit_amount = j["amount"].asDouble();
    w.commit_serial = j["serial"].asUInt();
    return balance(w.held) && balance(w.pooled) && balance(w.commit_amount);
}
bool ReadInt(const Json::Value& v, int min, int max, int& result) {
    if (!v.isInt() || v.asInt() < min || v.asInt() > max) return false;
    result = v.asInt(); return true;
}
bool ReadRewardOrder(const Json::Value& order, std::vector<std::string>& keys) {
    // All weapons plus stats, recovery, armor, race, recruit and ally training.
    if (!order.isArray() || order.size() > 4096) return false;
    std::set<std::string> unique, categories;
    for (const auto& key : order) {
        if (!key.isString() || key.asString().empty() || key.asString().size() > 96 ||
            !unique.insert(key.asString()).second) return false;
        const std::string category = WeaponCategories::OldPurchase(key.asString());
        if (categories.insert(category).second) keys.push_back(category);
    }
    return true;
}
bool ReadDamageRanks(const Json::Value& row, StatRanks& ranks) {
    if (row.isMember("bone_damage") || row.isMember("flesh_damage"))
        return ReadInt(row["bone_damage"], 0, kMaxRank, ranks.bone_damage) &&
               ReadInt(row["flesh_damage"], 0, kMaxRank, ranks.flesh_damage);
    if (!ReadInt(row["damage"], 0, kMaxRank, ranks.bone_damage)) return false;
    ranks.flesh_damage = ranks.bone_damage;
    return true;
}
}
Json::Value Snapshot() {
    if (Net()->IsHosting()) FinishRemoteRestores();
    Json::Value j;
    j["checkpoints"] = Json::Value(Json::arrayValue);
    for (const auto& mark : run.checkpoints) {
        Json::Value row;
        row["source"] = mark.first;
        row["step"] = mark.second;
        j["checkpoints"].append(row);
    }
    for (const auto& sale : run.sold_authored) {
        Json::Value row;
        row["item"] = sale.first;
        row["weapon"] = sale.second;
        j["sold_map_weapons"].append(row);
    }
    j["v"] = 3; j["shop"] = run.shop_generation; j["attempt"] = run.attempt;
    j["profile_run"] = run.profile_run_id;
    j["profile_event"] = run.profile_event;
    j["profile_completion"] = run.profile_completion;
    if (run.restrict_upgrades) {
        j["map_upgrades"] = Json::Value(Json::arrayValue);
        for (const auto& key : run.map_upgrades) j["map_upgrades"].append(key);
    }
    j["completed_rounds"] = run.completed_rounds;
    j["reward_generation"] = run.reward_generation;
    j["round_completed"] = run.round_completed; j["match_completed"] = run.match_completed;
    if (run.survival_battery > 0) {
        j["survival_battery"] = run.survival_battery;
        j["survival_clears"] = Json::Value(Json::arrayValue);
        for (const auto& clear : run.survival_clears) {
            Json::Value row;
            row["team"] = clear.first; row["wave"] = clear.second;
            j["survival_clears"].append(row);
        }
    }
    j["open"] = run.open; j["final"] = run.final; j["loop"] = run.loop;
    j["ms_left"] = run.deadline_ms > 0 ? std::max(0., run.deadline_ms-OnlineClockMs()) : -1.;
    j["ready"] = Json::Value(Json::arrayValue);
    for (auto id : run.ready) j["ready"].append(id);
    j["xp_scales"] = Json::Value(Json::arrayValue);
    for (const auto& entry : run.xp_scales) {
        Json::Value row = entry.second;
        row["player"] = entry.first;
        j["xp_scales"].append(row);
    }
    j["players"] = Json::Value(Json::arrayValue);
    for (const auto& p : run.players) {
        const auto& a = p.second; const auto& b = a.build;
        Json::Value r;
        r["id"] = p.first; r["w"] = WriteWallet(a.wallet);
        if (a.handicap) r["handicap"] = true;
        if (a.race >= 0) r["race"] = a.race;
        if (a.first_race >= 0) r["first_race"] = a.first_race;
        if (a.equipped_race >= 0) r["equipped_race"] = a.equipped_race;
        for (const auto& hand : a.reward_hands) {
            Json::Value saved;
            saved["generation"] = hand.first;
            for (const auto& key : hand.second) saved["order"].append(key);
            r["reward_hands"].append(saved);
        }
        r["health"] = b.health; r["bone_damage"] = b.bone_damage; r["flesh_damage"] = b.flesh_damage; r["speed"] = b.speed;
        r["fall_resistance"] = b.fall_resistance;
        r["stealth_passive"] = b.stealth_passive; r["stealth_active"] = b.stealth_active;
        r["luck"] = b.luck;
        for (int tier : b.armor) r["armor"].append(tier);
        uint64_t mask = 0;
        for (const auto& category : a.weapons) {
            const int old = WeaponCategories::ToLegacy(category);
            if (old >= 0) mask |= uint64_t{1} << old;
            else r["extra_weapons"].append(category);
        }
        r["weapons"] = Json::UInt64(mask);
        const int main = WeaponCategories::ToLegacy(b.main_weapon), off = WeaponCategories::ToLegacy(b.offhand_weapon);
        r["big"] = std::max(0, main); r["dagger_variant"] = std::max(0, off); r["dagger"] = !b.offhand_weapon.empty();
        if (main < 0 && !b.main_weapon.empty()) r["main_category"] = b.main_weapon;
        if (off < 0 && !b.offhand_weapon.empty()) r["offhand_category"] = b.offhand_weapon;
        r["ai"] = b.ai; r["trained"] = b.trained; r["recruited"] = b.recruited; r["owner"] = b.owner;
        j["players"].append(r);
    }
    j["teams"] = Json::Value(Json::arrayValue);
    for (const auto& row : run.teams) {
        const auto& t = row.second;
        Json::Value r;
        r["team"] = row.first; r["w"] = WriteWallet(t.wallet); r["leader"] = t.leader;
        r["recipient"] = t.last_recipient; r["recruits"] = t.recruits;
        r["health"] = t.health; r["bone_damage"] = t.bone_damage; r["flesh_damage"] = t.flesh_damage; r["speed"] = t.speed;
        r["fall_resistance"] = t.fall_resistance;
        r["stealth_passive"] = t.stealth_passive; r["stealth_active"] = t.stealth_active;
        r["luck"] = t.luck;
        j["teams"].append(r);
    }
    return j;
}
bool Restore(const Json::Value& j, bool validate_only) {
    if (!j.isObject() || !j["v"].isInt() || j["v"].asInt() < 1 || j["v"].asInt() > 3 ||
        !j["players"].isArray() || j["players"].size() > 8192 || !j["teams"].isArray() || j["teams"].size() > 8192 ||
        !j["ready"].isArray() || j["ready"].size() > 8192 || !j["shop"].isUInt() || !j["attempt"].isUInt() ||
        !j["open"].isBool() || !j["final"].isBool() || !j["loop"].isBool() || !j["ms_left"].isNumeric()) return false;
    Run next;
    if (j.isMember("map_upgrades")) {
        const auto& keys = j["map_upgrades"];
        if (!keys.isArray() || keys.size() > 8192) return false;
        for (const auto& key : keys) {
            if (!key.isString()) return false;
            const auto id = key.asString();
            if (id.empty() || id.size() > 80 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:") != std::string::npos ||
                !next.map_upgrades.insert(id).second) return false;
        }
        next.restrict_upgrades = true;
        if (next.map_upgrades.erase("damage")) {
            next.map_upgrades.insert("bone_damage");
            next.map_upgrades.insert("flesh_damage");
        }
    }
    if (j.isMember("profile_run")) {
        if (!j["profile_run"].isString() || j["profile_run"].asString().size() > 96 ||
            !j["profile_event"].isString() || j["profile_event"].asString().size() > 96 || !j["profile_completion"].isUInt()) return false;
        next.profile_run_id = j["profile_run"].asString();
        next.profile_event = j["profile_event"].asString();
        next.profile_completion = j["profile_completion"].asUInt();
    }
    if (j.isMember("survival_battery") || j.isMember("survival_clears")) {
        const auto& waves = j["survival_clears"];
        if (!j["survival_battery"].isInt() || j["survival_battery"].asInt() < 1 || !waves.isArray() || waves.size() > 8193) return false;
        next.survival_battery = j["survival_battery"].asInt();
        for (const auto& clear : waves) {
            if (!clear.isObject() || !clear["team"].isInt() || clear["team"].asInt() < 0 ||
                !clear["wave"].isInt() || clear["wave"].asInt() < 0 || clear["wave"].asInt() > 8192 ||
                !next.survival_clears.emplace(clear["team"].asInt(), clear["wave"].asInt()).second) return false;
        }
    }
    if (j.isMember("checkpoints")) {
        const auto& marks = j["checkpoints"];
        if (!marks.isArray() || marks.size() > 8192) return false;
        for (const auto& mark : marks) {
            int step;
            if (!mark.isObject() || !mark["source"].isInt() || mark["source"].asInt() < -1 ||
                !ReadInt(mark["step"], 1, 4096, step) || !next.checkpoints.emplace(mark["source"].asInt(), step).second) return false;
        }
    }
    next.shop_generation = j["shop"].asUInt(); next.attempt = j["attempt"].asUInt();
    // Older saves counted completed levels/matches/batteries, but not individual rounds.
    if (j.isMember("completed_rounds") && !j["completed_rounds"].isUInt()) return false;
    next.completed_rounds = j.get("completed_rounds", next.shop_generation).asUInt();
    const bool legacy_rewards = j["v"].asInt() < 3;
    if (legacy_rewards) next.reward_generation = next.shop_generation;
    else {
        if (!j["reward_generation"].isUInt() || !j["round_completed"].isBool() || !j["match_completed"].isBool()) return false;
        next.reward_generation = j["reward_generation"].asUInt();
        next.round_completed = j["round_completed"].asBool();
        next.match_completed = j["match_completed"].asBool();
    }
    next.open = j["open"].asBool(); next.final = j["final"].asBool(); next.loop = j["loop"].asBool();
    if (j.isMember("rolls_only") && !j["rolls_only"].isBool()) return false;
    double left = j["ms_left"].asDouble();
    if (!std::isfinite(left) || left < -1 || left > 120001) return false;
    if (left >= 0) next.deadline_ms = OnlineClockMs() + left;
    for (const auto& seat : j["ready"]) { if (!seat.isUInt()) return false; next.ready.insert(seat.asUInt()); }
    if (j.isMember("xp_scales")) {
        const auto& scales = j["xp_scales"];
        if (!scales.isArray() || scales.size() > 8192) return false;
        for (const auto& row : scales) {
            if (!row.isObject() || !row["player"].isUInt() || row["player"].asUInt() == invalid ||
                !row["world_remaining"].isNumeric() || !row["encounter_expected"].isNumeric()) return false;
            const double world = row["world_remaining"].asDouble(), encounter = row["encounter_expected"].asDouble();
            if (!std::isfinite(world) || !std::isfinite(encounter) || world < 0 || encounter < 0 ||
                !row["encounter_label"].isString() ||
                row["encounter_label"].asString().size() > 128 || next.xp_scales.count(row["player"].asUInt())) return false;
            auto& scale = next.xp_scales[row["player"].asUInt()];
            scale = row;
            scale.removeMember("player");
        }
    }
    for (const auto& r : j["players"]) {
        if (!r.isObject() || !r["id"].isUInt() || !r["weapons"].isUInt64() || r["weapons"].asUInt64() >= (uint64_t{1} << WeaponCategories::kLegacyCount) ||
            !r["dagger"].isBool() || !r["trained"].isBool() || !r["recruited"].isBool() || !r["owner"].isUInt() ||
            !r["armor"].isArray() || r["armor"].size()!=9) return false;
        auto& a = next.players[r["id"].asUInt()]; auto& b = a.build;
        if (r.isMember("handicap") && !r["handicap"].isBool()) return false;
        a.handicap = r.get("handicap", false).asBool();
        if (!ReadInt(r.get("race", -1), -1, 4, a.race)) return false;
        if (r.isMember("first_race") && !ReadInt(r["first_race"], 0, a.race, a.first_race)) return false;
        if (r.isMember("equipped_race") && (a.first_race < 0 ||
            !ReadInt(r["equipped_race"], a.first_race, a.race, a.equipped_race))) return false;
        if (legacy_rewards && r.isMember("free_rewards")) {
            // Older versions offered one hand per shop. Keep a pending choice
            // when resuming an old save and leave already-claimed hands empty.
            std::vector<std::string> keys;
            if (!next.open || !next.shop_generation || !ReadRewardOrder(r["free_rewards"], keys)) return false;
            if (!keys.empty()) a.reward_hands.emplace(next.shop_generation, std::move(keys));
        } else if (!legacy_rewards && r.isMember("reward_hands")) {
            if (!r["reward_hands"].isArray() || r["reward_hands"].size() > next.reward_generation) return false;
            for (const auto& hand : r["reward_hands"]) {
                if (!hand.isObject() || !hand["generation"].isUInt()) return false;
                const unsigned generation = hand["generation"].asUInt();
                std::vector<std::string> keys;
                if (!generation || generation > next.reward_generation || !ReadRewardOrder(hand["order"], keys) ||
                    keys.empty() || !a.reward_hands.emplace(generation, std::move(keys)).second) return false;
            }
        }
        int old_main, old_offhand;
        if (!ReadWallet(r["w"],a.wallet) || !ReadInt(r["health"],0,kMaxRank,b.health) ||
            !ReadDamageRanks(r,b) || !ReadInt(r["speed"],0,kMaxRank,b.speed) ||
            !ReadInt(r.get("fall_resistance",0),0,kMaxRank,b.fall_resistance) ||
            !ReadInt(r.get("stealth_passive",0),0,kMaxRank,b.stealth_passive) ||
            !ReadInt(r.get("stealth_active",0),0,kMaxRank,b.stealth_active) ||
            !ReadInt(r.get("luck",0),0,kMaxRank,b.luck) ||
            !ReadInt(r.get("dagger_variant",0),0,WeaponCategories::kLegacyCount-1,old_offhand) ||
            !ReadInt(r["big"],0,WeaponCategories::kLegacyCount-1,old_main) || !ReadInt(r["ai"],0,3,b.ai)) return false;
        for (unsigned i = 0; i < b.armor.size(); ++i) {
            int value;
            if (!ReadInt(r["armor"][i], 0, j["v"].asInt() == 1 ? ArmorProtection::Dog : ArmorProtection::Grey, value)) return false;
            b.armor[i] = ArmorProtection::RestoredShopMaterial(i, value, j["v"].asInt() == 1);
        }
        for (int bit = 0; bit < WeaponCategories::kLegacyCount; ++bit)
            if (r["weapons"].asUInt64() & (uint64_t{1} << bit)) a.weapons.insert(WeaponCategories::FromLegacy(bit));
        if (r.isMember("extra_weapons")) {
            const auto& extra = r["extra_weapons"];
            if (!extra.isArray() || extra.size() > 4096) return false;
            for (const auto& category : extra) {
                if (!category.isString() || category.asString().empty() || category.asString().size() > 80) return false;
                a.weapons.insert(category.asString());
            }
        }
        b.main_weapon = old_main ? WeaponCategories::FromLegacy(old_main) : "";
        b.offhand_weapon = r["dagger"].asBool() ? WeaponCategories::FromLegacy(old_offhand) : "";
        for (auto key : {"main_category", "offhand_category"}) {
            if (!r.isMember(key)) continue;
            if (!r[key].isString() || !a.weapons.count(r[key].asString())) return false;
            (std::string(key) == "main_category" ? b.main_weapon : b.offhand_weapon) = r[key].asString();
        }
        b.trained = r["trained"].asBool();
        b.recruited = r["recruited"].asBool(); b.owner = r["owner"].asUInt();
    }
    for (const auto& r : j["teams"]) {
        if (!r.isObject() || !r["team"].isInt() || r["team"].asInt() < 1 ||
            !r["leader"].isUInt() || !r["recipient"].isUInt()) return false;
        auto& t = next.teams[r["team"].asInt()];
        if (!ReadWallet(r["w"],t.wallet) || !ReadInt(r["recruits"],0,100000,t.recruits) ||
            !ReadInt(r["health"],0,kMaxRank,t.health) || !ReadDamageRanks(r,t) ||
            !ReadInt(r["speed"],0,kMaxRank,t.speed) ||
            !ReadInt(r.get("fall_resistance",0),0,kMaxRank,t.fall_resistance)) return false;
        if (!ReadInt(r.get("stealth_passive",0),0,kMaxRank,t.stealth_passive) ||
            !ReadInt(r.get("stealth_active",0),0,kMaxRank,t.stealth_active) ||
            !ReadInt(r.get("luck",0),0,kMaxRank,t.luck)) return false;
        t.leader = r["leader"].asUInt(); t.last_recipient = r["recipient"].asUInt();
    }
    if (j.isMember("sold_map_weapons")) {
        const auto& sales = j["sold_map_weapons"];
        if (!sales.isArray() || sales.size() > 65536) return false;
        for (const auto& sale : sales) {
            if (!sale.isObject() || !sale["item"].isInt() || sale["item"].asInt() < 0 ||
                !sale["weapon"].isString() || (!OnlineUtility::IsSafeAssetPath(sale["weapon"].asString()) &&
                WeaponCategories::OldItemPath(sale["weapon"].asString()).empty()) ||
                !next.sold_authored.emplace(sale["item"].asInt(),sale["weapon"].asString()).second) return false;
        }
    }
    // Older builds also paused for profile rolls. Only a final campaign
    // decision or a pending Survival reward still needs that intermission.
    if (next.open && j.get("rolls_only", false).asBool()) {
        const bool pending_reward = next.survival_battery > 0 &&
            std::any_of(next.players.begin(), next.players.end(), [](const auto& player) {
                return !player.second.reward_hands.empty();
            });
        next.open = next.final || pending_reward;
        if (!next.open) {
            next.ready.clear();
            next.deadline_ms = 0;
        }
    }
    if (!validate_only) {
        pending_restores.clear();
        run = std::move(next);
        restore_sales = !run.sold_authored.empty();
    }
    return true;
}
}
