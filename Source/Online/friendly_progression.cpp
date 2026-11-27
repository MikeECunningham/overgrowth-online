// Friendly progression is owned by the host. Combat peers report outcomes only
// for victims they simulate, and shops transact against the host's wallet.
#include <Online/friendly_progression.h>
#include <Online/session_mods.h>
#include <Online/armor_protection.h>
#include <Online/progression_economy.h>
#include <Online/campaign_difficulty.h>
#include <Online/online.h>
#include <Online/online_utility.h>
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
const std::array<Weapon, kWeaponCount>& Weapons() {
    static const std::array<Weapon, kWeaponCount> weapons = {{
        {"dagger", "Offhand dagger", "Data/Items/rabbit_weapons/rabbit_knife.xml", "Object/rabbit_knife", "dagger", 0, 2, true},
        {"throwing_knife", "Rabbit throwing knife", "Data/Items/rabbit_weapons/rabbit_throwing_knife.xml", "Object/rabbit_throwing_knife", "dagger", 5, 2, true},
        {"flint_knife", "Flint knife", "Data/Items/flint_knife.xml", "Object/flint_knife", "dagger", 6, 2, true},
        {"flint_knife_c2", "Flint knife (painted)", "Data/Items/flint_knife_c2.xml", "Object/flint_knife_c2", "dagger", 7, 2, true},
        {"dog_knife", "Dog knife", "Data/Items/DogWeapons/DogKnife.xml", "Object/DogKnife", "dagger", 8, 2, true},
        {"main_gauche", "Main gauche", "Data/Items/MainGauche.xml", "Object/wcatknife", "dagger", 9, 2, true},
        {"cinder_knife", "Cinderbreathe knife", "Data/Items/cinderbreathe_weapons/cinderbreatheknife.xml", "Object/rabbit_knife", "dagger", 10, 2, true},
        {"gabenife", "Gabe knife", "Data/Items/gabenife.xml", "Object/gabenife", "dagger", 11, 2, true},
        {"rat_blade", "Rat throwing blade", "Data/Items/rat_weapons/rat_throwing_blade.xml", "Object/rat_throwing_blade", "dagger", 12, 2, true},
        {"staff", "Staff", "Data/Items/staffbasic.xml", "Old & Nonworking/staffbasic", "staff", 3, 2, false},
        {"stick", "Stick", "Data/Items/stick.xml", "Static Objects/stick", "stick", 13, 1, false},
        {"kendo", "Kendo stick", "Data/Items/kendostick_1h.xml", "Old & Nonworking/staffbasic", "staff", 14, 2, false},
        {"torch", "Torch", "Data/Items/torch.xml", "Interactive Objects/torch", "dagger", 15, 2, false},
        {"arrow", "Arrow", "Data/Items/StandardArrow.xml", "Static Objects/stick", "stick", 16, 1, false},
        {"rat_machete", "Rat machete", "Data/Items/rat_weapons/rat_machete.xml", "Object/rat_machete", "machete", 17, 2, false},
        {"spear", "Spear", "Data/Items/DogWeapons/DogSpear.xml", "Object/DogSpear", "spear", 2, 3, false},
        {"glaive", "Dog glaive", "Data/Items/DogWeapons/DogGlaive.xml", "Object/DogGlaive", "spear", 18, 3, false},
        {"cinder_spear", "Cinderbreathe spear", "Data/Items/cinderbreathe_weapons/cinderbreathespear.xml", "Object/DogSpear", "spear", 19, 3, false},
        {"cinder_glaive", "Cinderbreathe glaive", "Data/Items/cinderbreathe_weapons/cinderbreatheglaive.xml", "Object/DogSpear", "spear", 20, 3, false},
        {"sword", "Sword", "Data/Items/DogWeapons/DogSword.xml", "Object/DogSword", "sword", 1, 3, false},
        {"rapier", "Cat rapier", "Data/Items/Rapier.xml", "Object/Rapier", "sword", 21, 3, false},
        {"cinder_sword", "Cinderbreathe sword", "Data/Items/cinderbreathe_weapons/cinderbreathesword.xml", "Object/DogSword", "sword", 22, 3, false},
        {"dog_hammer", "Dog hammer", "Data/Items/DogWeapons/DogHammer.xml", "Object/DogHammer", "hammer", 23, 3, false},
        {"pole_hammer", "Dog polehammer", "Data/Items/DogHammer.xml", "Object/DogHammer", "hammer", 24, 3, false},
        {"rabbit_catcher", "Rabbit catcher", "Data/Items/DogWeapons/RabbitCatcher.xml", "Object/RabbitCatcher", "spear", 25, 3, false},
        {"glass_macuahuitl", "Glass macuahuitl", "Data/Items/macuahuitl_glass.xml", "Object/macuahuitl_glass", "sword", 26, 3, false},
        {"metal_macuahuitl", "Metal macuahuitl", "Data/Items/macuahuitl_metal.xml", "Object/macuahuitl_metal", "sword", 27, 3, false},
        {"broadsword", "Broadsword", "Data/Items/DogWeapons/DogBroadSword.xml", "Object/DogBroadSword", "broadsword", 4, 4, false},
        {"bastard_sword", "Bastard sword", "Data/Items/Bastard.xml", "O2/Bastard", "broadsword", 28, 4, false},
        {"cinder_broadsword", "Cinderbreathe broadsword", "Data/Items/cinderbreathe_weapons/cinderbreathebroadsword.xml", "Object/DogBroadSword", "broadsword", 29, 4, false},
        {"dog_scabbard", "Dog sword scabbard", "Data/Items/DogWeapons/DogSwordScabbard.xml", "Object/dog_sword_scabbard", "stick", 30, 1, false},
        {"cat_scabbard", "Cat rapier scabbard", "Data/Items/cat_weapons/cat_rapier_scabbard.xml", "Object/cat_rapier_scabbard", "stick", 31, 1, false},
        {"cinder_scabbard", "Cinderbreathe scabbard", "Data/Items/cinderbreathe_weapons/cinderbreatheswordscabbard.xml", "Object/dog_sword_scabbard", "stick", 32, 1, false},
        {"tool_awl", "Dog tool awl", "Data/Items/gear/dogtools/dogtoolawl.xml", "Object/DogToolAwl", "tool", 33, 1, false},
        {"tool_chisel", "Dog tool chisel", "Data/Items/gear/dogtools/dogtoolchisel.xml", "Object/DogToolChisel", "tool", 34, 1, false},
        {"tool_hammer", "Dog tool hammer", "Data/Items/gear/dogtools/dogtoolhammer.xml", "Object/DogToolHammer", "tool", 35, 1, false},
    }};
    return weapons;
}

namespace {
using ID = uint32_t;
constexpr ID invalid = UINT32_MAX;
const char* const regions[] = {"Chest", "Back", "Head", "Left forearm", "Right forearm", "Left shoulder", "Right shoulder", "Left thigh", "Right thigh"};
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
    int race = -1;
    uint64_t weapons = 0;
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
bool PaidShop() { return Net()->ModeSetting("progression_shop") != 0; }
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
int AssignedRace(ID id, bool bot) {
    if (Enabled()) {
        auto account = run.players.find(id);
        if (account != run.players.end() && account->second.race >= 0) return account->second.race;
        if (bot) return 1;
    }
    if (!Net()->IsSurvivalMode()) return -1;
    return bot ? 1 : std::min(4, std::max(0, Net()->ModeSetting("survival_starting_race")));
}
int CurrentRace(const PlayerState& seat) {
    int assigned = AssignedRace(seat.player_id, seat.is_bot);
    if (assigned >= 0) return assigned;
    auto* actor = Actor(seat.object_id);
    return actor ? NaturalRace(*actor) : -1;
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
    stats["health_rank"] = build.health; stats["damage_rank"] = build.damage; stats["speed_rank"] = build.speed;
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
    stats["damage"] = value("p_attack_damage_mult", 1);
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
const Weapon* FindWeapon(const std::string& key) {
    for (const auto& weapon : Weapons()) if (key == weapon.key) return &weapon;
    return nullptr;
}
int WeaponPrice(const Weapon& weapon) {
    return static_cast<int>(Rule(std::string("weapon_") + weapon.price_family));
}
// An inventory action names the item as well as its socket, so a stale click
// cannot sell a replacement after a throw, pickup, or a second sale.
struct CarriedWeapon {
    ItemObject* item = nullptr;
    const Weapon* catalog = nullptr;
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
    for (const auto& weapon : Weapons()) {
        if (item->obj_file != weapon.path) continue;
        result.catalog = &weapon;
        result.key = "sell_" + std::to_string(slot) + "_" + std::to_string(Net()->GetOriginalID(id)) + "_" + weapon.key;
        break;
    }
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
                account.weapons &= ~held.catalog->Ownership();
                if (held.catalog->offhand && account.build.dagger_variant == held.catalog->selection) {
                    account.build.dagger = false;
                    account.build.dagger_variant = 0;
                } else if (!held.catalog->offhand && account.build.big_weapon == held.catalog->selection)
                    account.build.big_weapon = 0;
            }
        } else if (!held.item->exclude_from_save) {
            // Authored map items return with their original IDs on a cold load.
            run.sold_authored[Net()->GetOriginalID(id)] = held.catalog->key;
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
    {"damage", "Attack damage", kDamageGain, "+", "% of base attack damage", &StatRanks::damage},
    {"speed", "Movement speed", kSpeedGain, "+", "% of base movement speed", &StatRanks::speed},
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
            detail << stat.prefix << std::round(stat.gains.Next(rank) * 10000) / 100 << stat.benefit;
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
        int race = CurrentRace(*Seat(id));
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
    for (const auto& weapon : Weapons()) {
        for (ID id : choices) {
            if (run.players[id].weapons & weapon.Ownership()) continue;
            offer(weapon.key, weapon.label, "Unlock for " + Seat(id)->playername,
                  WeaponPrice(weapon), weapon.rarity, id);
            break;
        }
    }
    if (target.is_bot) {
        static const char* const prices[] = {"ai_hardcore", "ai_expert", "ai_nightmare"};
        static const char* names[] = {"Hardcore AI", "Expert AI", "Nightmare AI"};
        if (build.ai < 3) offer("ai", names[build.ai], build.ai == 2 ? "Earns XP and trains its own stats between fights." : "Train this ally.", static_cast<int>(Rule(prices[build.ai])), build.ai + 2, target.player_id);
    } else if (!choices.empty() && Net()->HasSpaceForLobbySlot()) {
        offer("recruit","Recruit an ally","A new ally with Casual AI.", RecruitPrice(team.recruits, Rule("recruit_base"), Rule("recruit_growth")), 4, choices.front());
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
    if (stat) {
        if (Shared()) ++(team.*stat->rank);
        else ++(account.build.*stat->rank);
    } else if (key == "race_upgrade") {
        account.race = offer.next_race;
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
    } else if (const auto* weapon = FindWeapon(key)) {
        account.weapons |= weapon->Ownership();
        if (weapon->offhand) {
            account.build.dagger_variant = weapon->selection;
            account.build.dagger = true;
        } else account.build.big_weapon = weapon->selection;
    } else {
        const unsigned region = static_cast<unsigned>(key.back() - '0'); // key came from the catalog
        account.build.armor[region] = offer.armor_material;
    }
    if (Shared() && (!target.is_bot || offer.armor_material) && !stat) team.last_recipient = offer.recipient;
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

// The card's place in the saved draw belongs to this particular armor or training
// tier. A teammate buying the last copy reveals the next eligible card in order.
std::string RewardKey(const Offer& offer) {
    if (offer.armor_material || offer.next_race >= 0 || offer.id == "ai") return offer.id + ":" + std::to_string(offer.rarity);
    return offer.id;
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
        }
        break;
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
    for (const auto& hand : hands) {
        for (const auto& key : hand.second) {
            const auto item = std::find_if(pool.begin(), pool.end(), [&](const Offer& o) { return RewardKey(o) == key; });
            if (item != pool.end()) cards.push_back(*item);
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
    if (const auto* weapon = FindWeapon(offer.id)) Net()->ProgressionWeaponAwarded(offer.recipient, weapon->offhand);
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
            int* lowest = &build.health;
            if (build.damage < *lowest) lowest = &build.damage;
            if (build.speed < *lowest) lowest = &build.speed;
            if (build.stealth_passive < *lowest) lowest = &build.stealth_passive;
            if (build.stealth_active < *lowest) lowest = &build.stealth_active;
            if (*lowest >= kMaxRank || account.wallet.held < UpgradePrice(*lowest)) break;
            account.wallet.held -= UpgradePrice(*lowest);
            ++*lowest;
        }
    }
}
int SecondsLeft() {
    return run.deadline_ms <= 0 ? -1 : int(std::max(0.0, std::ceil((run.deadline_ms - OnlineClockMs()) / 1000.0)));
}
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
    const int race = AssignedRace(id, bot);
    if (race < 0 || !avatar.as_context || !avatar.rigged_object()) return false;
    // The replicated Species parameter can arrive before the new skeleton.
    // Keep authored skins when the equipped body and its gameplay species agree.
    const auto& desired = races[race];
    if (avatar.rig_character_path != desired.character &&
        (NaturalRace(avatar) != race || avatar.character_script_getter.GetTag("species") != desired.species))
        avatar.ApplyNetworkCharacter(desired.character);
    return true;
}
int SurvivalShopSeconds() {
    const int index = Net()->ModeSetting("progression_timer");
    if (index == 0) return -1;
    return index > 0 && index < 6 ? timers[index] : 0;
}
bool LoopChosen() { return Enabled() && run.final && run.loop; }
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
void NewSession(bool keep_slot_handicaps) {
    Run fresh;
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
void ArenaWin(const std::vector<int>& winners, bool match, const std::vector<int>* participants) {
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

bool BeginShop(bool final) {
    if (!Enabled() || !Net()->IsHosting() || Net()->LobbyWaiting()) return false;
    if (run.open) return true;
    if (!Arena() && !Net()->IsSurvivalMode() && Net()->IsCampaignPlayMode()) run.CompleteRound();
    if (!Net()->IsSurvivalMode()) Checkpoint(); // Survival banks each team's own wave.
    if (SceneGraph* graph = Engine::Instance()->GetSceneGraph()) SampleXPScales(graph);
    if (PaidShop()) TrainBots();
    // The existing generation also counts completions in older save cards.
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
void Update() {
    if (!Net()->IsHosting() || !Net()->online_session || Net()->LobbyWaiting() || !Enabled()) return;
    FinishRemoteRestores();
    if (restore_sales && Net()->host_started_level && Net()->SendingRemovePackages() && Engine::Instance()->IsLevelLoaded()) {
        SceneGraph* scene = Engine::Instance()->GetSceneGraph();
        for (const auto& sale : run.sold_authored) {
            const int id = Net()->GetObjectID(sale.first);
            const auto* expected = FindWeapon(sale.second);
            auto* item = scene->DoesObjectWithIdExist(id) ? scene->GetObjectFromID(id) : nullptr;
            if (expected && item && item->GetType() == _item_object && !item->exclude_from_save && item->obj_file == expected->path)
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
        if (!waiting || expired) {
            if (Net()->IsSurvivalMode()) for (auto& account : run.players) account.second.reward_hands.clear();
            run.open = false;
            run.deadline_ms = 0;
            spawns.clear(); // Arena reuses the same object IDs for the next match.
            ++run.attempt;
            Publish();
        }
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
    auto& w = Balance(*buyer);
    v["held"] = w.held;
    v["pooled"] = w.pooled;
    v["commit_amount"] = w.commit_amount;
    v["commit_serial"] = w.commit_serial;
    v["shop_open"] = run.open;
    v["shop_enabled"] = PaidShop();
    v["xp_scale"] = run.xp_scales[actor];
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
    v["ready"] = run.ready.count(actor) != 0;
    v["seconds_left"] = SecondsLeft();
    v["final"] = run.final;
    v["loop"] = run.loop;
    v["can_loop"] = run.final && Net()->IsLobbyHost(actor);
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
    for (const auto& weapon : Weapons())
        v[std::string("owned_") + weapon.key] = bool(account.weapons & weapon.Ownership());
    v["dagger_variant"] = account.build.dagger_variant;
    v["big_weapon"] = account.build.big_weapon;
    v["dagger"] = account.build.dagger;
    v["can_equip"] = subject == buyer || (subject->is_bot && v["can_buy"].asBool());
    static const char* const slots[] = {"Left hand", "Right hand", "Left belt", "Right belt"};
    v["inventory"] = Json::Value(Json::arrayValue);
    for (int slot = 0; slot < 4; ++slot) {
        const auto held = Carried(*subject, slot);
        Json::Value row;
        row["slot"] = slots[slot];
        row["item"] = held.item ? static_cast<int>(Net()->GetOriginalID(held.item->GetID())) : -1;
        if (held.catalog) {
            row["label"] = held.catalog->label;
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
    } else if (action == "equip" && (target == actor || (subject->is_bot && may_buy))) {
        auto& acc = run.players[target];
        if (key == "none") acc.build.big_weapon = 0;
        else {
            const auto* weapon = FindWeapon(key);
            if (!weapon || !(acc.weapons & weapon->Ownership())) return false;
            if (weapon->offhand) {
                acc.build.dagger = !acc.build.dagger || acc.build.dagger_variant != weapon->selection;
                acc.build.dagger_variant = weapon->selection;
            } else acc.build.big_weapon = weapon->selection;
        }
    } else return false;
    Publish();
    return true;
}
bool Act(ID actor, ID target, const std::string& action, const std::string& key, unsigned expected_reward_generation) {
    const auto* owner = Seat(actor);
    if (!Enabled() || Net()->LobbyWaiting() || !owner || owner->peer_id != Net()->LocalPeerID() || owner->is_bot) return false;
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
    if (!order.isArray() || order.size() > Weapons().size() + 20) return false;
    std::set<std::string> unique;
    for (const auto& key : order) {
        if (!key.isString() || key.asString().empty() || key.asString().size() > 32 ||
            !unique.insert(key.asString()).second) return false;
        keys.push_back(key.asString());
    }
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
        for (const auto& hand : a.reward_hands) {
            Json::Value saved;
            saved["generation"] = hand.first;
            for (const auto& key : hand.second) saved["order"].append(key);
            r["reward_hands"].append(saved);
        }
        r["health"] = b.health; r["damage"] = b.damage; r["speed"] = b.speed;
        r["stealth_passive"] = b.stealth_passive; r["stealth_active"] = b.stealth_active;
        r["luck"] = b.luck;
        for (int tier : b.armor) r["armor"].append(tier);
        r["weapons"] = Json::UInt64(a.weapons); r["dagger_variant"] = b.dagger_variant; r["big"] = b.big_weapon; r["dagger"] = b.dagger;
        r["ai"] = b.ai; r["trained"] = b.trained; r["recruited"] = b.recruited; r["owner"] = b.owner;
        j["players"].append(r);
    }
    j["teams"] = Json::Value(Json::arrayValue);
    for (const auto& row : run.teams) {
        const auto& t = row.second;
        Json::Value r;
        r["team"] = row.first; r["w"] = WriteWallet(t.wallet); r["leader"] = t.leader;
        r["recipient"] = t.last_recipient; r["recruits"] = t.recruits;
        r["health"] = t.health; r["damage"] = t.damage; r["speed"] = t.speed;
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
        if (!r.isObject() || !r["id"].isUInt() || !r["weapons"].isUInt64() || r["weapons"].asUInt64() > kWeaponOwnershipMask ||
            !r["dagger"].isBool() || !r["trained"].isBool() || !r["recruited"].isBool() || !r["owner"].isUInt() ||
            !r["armor"].isArray() || r["armor"].size()!=9) return false;
        auto& a = next.players[r["id"].asUInt()]; auto& b = a.build;
        if (r.isMember("handicap") && !r["handicap"].isBool()) return false;
        a.handicap = r.get("handicap", false).asBool();
        if (!ReadInt(r.get("race", -1), -1, 4, a.race)) return false;
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
        if (!ReadWallet(r["w"],a.wallet) || !ReadInt(r["health"],0,kMaxRank,b.health) ||
            !ReadInt(r["damage"],0,kMaxRank,b.damage) || !ReadInt(r["speed"],0,kMaxRank,b.speed) ||
            !ReadInt(r.get("stealth_passive",0),0,kMaxRank,b.stealth_passive) ||
            !ReadInt(r.get("stealth_active",0),0,kMaxRank,b.stealth_active) ||
            !ReadInt(r.get("luck",0),0,kMaxRank,b.luck) ||
            !ReadInt(r.get("dagger_variant",0),0,kWeaponCount-1,b.dagger_variant) ||
            !ReadInt(r["big"],0,kWeaponCount-1,b.big_weapon) || !ReadInt(r["ai"],0,3,b.ai)) return false;
        for (unsigned i = 0; i < b.armor.size(); ++i) {
            int value;
            if (!ReadInt(r["armor"][i], 0, j["v"].asInt() == 1 ? ArmorProtection::Dog : ArmorProtection::Grey, value)) return false;
            b.armor[i] = ArmorProtection::RestoredShopMaterial(i, value, j["v"].asInt() == 1);
        }
        a.weapons = r["weapons"].asUInt64(); b.dagger = r["dagger"].asBool(); b.trained = r["trained"].asBool();
        b.recruited = r["recruited"].asBool(); b.owner = r["owner"].asUInt();
    }
    for (const auto& r : j["teams"]) {
        if (!r.isObject() || !r["team"].isInt() || r["team"].asInt() < 1 ||
            !r["leader"].isUInt() || !r["recipient"].isUInt()) return false;
        auto& t = next.teams[r["team"].asInt()];
        if (!ReadWallet(r["w"],t.wallet) || !ReadInt(r["recruits"],0,100000,t.recruits) ||
            !ReadInt(r["health"],0,kMaxRank,t.health) || !ReadInt(r["damage"],0,kMaxRank,t.damage) ||
            !ReadInt(r["speed"],0,kMaxRank,t.speed)) return false;
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
                !sale["weapon"].isString() || !FindWeapon(sale["weapon"].asString()) ||
                !next.sold_authored.emplace(sale["item"].asInt(),sale["weapon"].asString()).second) return false;
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
