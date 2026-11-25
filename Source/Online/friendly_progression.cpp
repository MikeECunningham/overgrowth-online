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
#include <Objects/movementobject.h>
#include <Objects/itemobject.h>
#include <Internal/config.h>
#include <Internal/modloading.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace FriendlyProgression {
namespace {
using ID = uint32_t;
constexpr ID invalid = UINT32_MAX;
const char* const regions[] = {"Chest", "Back", "Head", "Left forearm", "Right forearm", "Left shoulder", "Right shoulder", "Left thigh", "Right thigh"};
const int timers[] = {0, 10, 15, 30, 60, 120};
struct Account {
    Wallet wallet;
    Build build;
    unsigned weapons = 0;
};
struct Team {
    Wallet wallet;
    ID leader = invalid, last_recipient = invalid;
    int recruits = 0;
    int health = 0, damage = 0, speed = 0;
    int stealth_passive = 0, stealth_active = 0;
};
struct Run {
    std::map<ID, Account> players;
    std::map<int, Team> teams;
    unsigned shop_generation = 0, attempt = 0;
    bool open = false, final = false, loop = false;
    double deadline_ms = 0;
    std::set<ID> ready;
    std::map<ID, Json::Value> xp_scales;
};
Run run;
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
bool Shared() { return Net()->ModeSetting("progression_shared") != 0; }
bool Combat() { return Arena() && Net()->ModeSetting("progression_combat") != 0; }
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
int ShopTeam(const PlayerState& seat) {
    if (!Arena() && !Net()->LobbyForceTeams()) return 1;
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
    const auto sample = Net()->EnemyDifficulty();
    auto factor = [&](const char* name) { return victim->ASHasVar(name) ? victim->ASGetFloatVar(name) : 1.f; };
    return Reward(BaseBounty(species, large), sample.base >= 0 ? sample.base : config["game_difficulty"].toNumber<double>(), gear,
        factor("campaign_vitality_mult"), factor("campaign_damage_mult"), sample.pressure);
}
void SampleXPScales(SceneGraph* graph) {
    const auto sample = Net()->EnemyDifficulty();
    for (const auto& row : Net()->online_session->player_states) {
        const auto& player = row.second;
        if (player.is_bot || !player.connected) continue;
        MovementObject* hero = Actor(player.object_id);
        if (!hero || !hero->as_context) continue;
        double world = 0, encounter = 0;
        auto enemy = [&](MovementObject* actor) {
            return actor && actor != hero && actor->as_context && actor->ASHasVar("species") && actor->ASHasVar("knocked_out") &&
                   !hero->ASOnSameTeam(actor);
        };
        for (Object* object : graph->movement_objects_) {
            auto* actor = static_cast<MovementObject*>(object);
            if (!enemy(actor) || actor->ASGetIntVar("knocked_out") != 0) continue;
            auto& spawn = Observe(actor);
            if (!spawn.defeated) world += Bounty(actor, spawn.equipment);
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
        scale["encounter_expected"] = encounter;
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
    stats["passive_range"] = 1.0 / (1.0 + .06 * build.stealth_passive);
    stats["active_range"] = 1.0 / (1.0 + .06 * build.stealth_active);
    MovementObject* actor = Actor(player.object_id);
    auto value = [&](const char* key, double fallback) {
        return actor && actor->ASHasVar(key) ? double(actor->ASGetFloatVar(key)) : fallback;
    };
    stats["durability"] = 1.0 / std::max(.00001, value("p_damage_multiplier", 1));
    stats["damage"] = value("p_attack_damage_mult", 1);
    stats["movement"] = value("p_speed_mult", 1) * (1.0 + .03 * build.speed);
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
void RewardKill(const PlayerState& killer, MovementObject* fighter, double reward) {
    const int team = ShopTeam(killer);
    if (Shared()) run.teams[team].wallet.Earn(reward, Arena());
    for (ID id : Members(team, true)) {
        const auto& p = *Seat(id);
        if (!Alive(p)) continue;
        auto* actor = Actor(p.object_id);
        if (!actor->ASOnSameTeam(fighter)) continue; // Arena can stage FFA among lobby friends.
        if (p.is_bot ? run.players[id].build.ai == 3 : !Shared()) run.players[id].wallet.Earn(reward, Arena());
    }
}
void ApplyHit(MovementObject* victim, MovementObject* attacker, float damage, bool defeat) {
    if (!victim || !attacker || !victim->as_context || !attacker->as_context ||
        victim == attacker || victim->ASOnSameTeam(attacker)) return;
    PlayerState killer, hurt;
    const bool from_roster = Net()->TryGetPlayerState(killer, attacker->GetID());
    const bool to_roster = Net()->TryGetPlayerState(hurt, victim->GetID());
    auto& spawn = Observe(victim);
    spawn.engaged = true;
    const double reward = Bounty(victim, spawn.equipment);
    if (defeat) {
        if (spawn.defeated) return;
        spawn.defeated = true;
        if (from_roster && !Combat()) RewardKill(killer, attacker, reward);
    } else if (Combat() && damage > 0 && !spawn.defeated) {
        // A full health bar dealt is worth a kill. Damage received costs 40%
        // of the same bounty; equal exchanges still allow gradual progress.
        auto combat_xp = [](const PlayerState& seat, double amount) {
            if (Shared()) run.teams[ShopTeam(seat)].wallet.Earn(amount, true);
            if (seat.is_bot ? run.players[seat.player_id].build.ai == 3 : !Shared())
                run.players[seat.player_id].wallet.Earn(amount, true);
        };
        if (from_roster) combat_xp(killer, reward * damage);
        if (to_roster) combat_xp(hurt, -.4 * Bounty(attacker, Observe(attacker).equipment) * damage);
    }
    dirty = true;
}
struct Offer {
    std::string id, label, description;
    int cost = 0;
    int rarity = 0;
    ID recipient = invalid;
    int armor_material = 0;
};
enum WeaponBit { Knife = 1, Sword = 2, Spear = 4, Staff = 8, Broadsword = 16 };
unsigned WeaponFlag(const std::string& key) {
    for (const auto& weapon : Weapons()) if (key == weapon.key) return weapon.ownership;
    return 0;
}
int WeaponSelection(const std::string& key) {
    for (const auto& weapon : Weapons()) if (key == weapon.key) return weapon.selection;
    return 0;
}
int* Stat(Build& b, const std::string& key) {
    if (key == "health") return &b.health;
    if (key == "damage") return &b.damage;
    if (key == "speed") return &b.speed;
    if (key == "stealth_passive") return &b.stealth_passive;
    if (key == "stealth_active") return &b.stealth_active;
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
std::vector<Offer> Catalog(const PlayerState& buyer, const PlayerState& target) {
    std::vector<Offer> offers;
    int team_id = ShopTeam(buyer);
    auto& team = run.teams[team_id];
    const bool shared = Shared();
    Build build = GetBuild(target.player_id);
    std::vector<ID> choices = shared && !target.is_bot ? Rotation(team_id) : std::vector<ID>{target.player_id};
    auto offer = [&](std::string id, std::string label, std::string desc, int cost, int rarity, ID recipient) {
        offers.push_back({id, label, desc, cost, rarity, recipient});
    };
    if (!target.is_bot) {
        for (const auto& k : {"health", "damage", "speed", "stealth_passive", "stealth_active"}) {
            const std::string key(k);
            const int rank = *Stat(build, key);
            const std::string label = key == "health" ? "Durability" : key == "damage" ? "Attack damage" : key == "speed" ? "Movement speed" :
                                      key == "stealth_passive" ? "Passive stealth" : "Active stealth";
            const std::string benefit = key == "health" ? "+8% durability" : key == "damage" ? "+6% damage" : key == "speed" ? "+3% movement speed" :
                                        key == "stealth_passive" ? "+6% concealment from sight and hearing while standing" :
                                                                   "+6% concealment from sight and hearing while crouched";
            if (rank < kMaxRank) offer(key,label,benefit + (shared ? " for every teammate." : "."),UpgradePrice(rank), 0, target.player_id);
        }
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
            if (run.players[id].weapons & weapon.ownership) continue;
            offer(weapon.key, weapon.label, "Unlock for " + Seat(id)->playername,
                  static_cast<int>(Rule(std::string("weapon_") + weapon.key)), weapon.rarity, id);
            break;
        }
    }
    if (target.is_bot) {
        static const char* const prices[] = {"ai_hardcore", "ai_expert", "ai_nightmare"};
        static const char* names[] = {"Hardcore AI", "Expert AI", "Nightmare AI"};
        if (build.ai < 3) offer("ai", names[build.ai], build.ai == 2 ? "Earns XP and trains its own stats between fights." : "Train this ally.", static_cast<int>(Rule(prices[build.ai])), build.ai + 2, target.player_id);
    } else if (!choices.empty()) {
        offer("recruit","Recruit an ally","A new ally with Casual AI.", RecruitPrice(team.recruits, Rule("recruit_base"), Rule("recruit_growth")), 4, choices.front());
    }
    return offers;
}
bool Buy(const PlayerState& buyer, const PlayerState& target, const std::string& key) {
    auto offers = Catalog(buyer, target);
    auto it = std::find_if(offers.begin(), offers.end(), [&](const Offer& o) { return o.id == key; });
    if (it == offers.end() || Balance(buyer).held < it->cost) return false;
    const int team_id = ShopTeam(buyer);
    auto& team = run.teams[team_id];
    auto& account = run.players[it->recipient];
    if (int* stat = Stat(account.build, key)) {
        if (Shared()) {
            if (key == "health") ++team.health;
            else if (key == "damage") ++team.damage;
            else if (key == "speed") ++team.speed;
            else if (key == "stealth_passive") ++team.stealth_passive;
            else if (key == "stealth_active") ++team.stealth_active;
        } else ++*stat;
    } else if (key == "recruit") {
        ID bot = Net()->AddLobbyBot("", Net()->LobbyHostPlayerID());
        if (bot == invalid) return false;
        // The recruit belongs to the recipient's lobby team, including when
        // mode-managed teams temporarily share the same campaign party.
        Net()->SetLobbyTeam(bot, Seat(it->recipient)->team, Net()->LobbyHostPlayerID());
        auto& build = run.players[bot].build;
        build.owner = it->recipient;
        build.recruited = build.trained = true;
        ++team.recruits;
    } else if (key == "ai") {
        ++account.build.ai;
        account.build.trained = true;
    } else if (unsigned flag = WeaponFlag(key)) {
        account.weapons |= flag;
        if (flag == Knife) account.build.dagger = true;
        else account.build.big_weapon = WeaponSelection(key);
    } else {
        const unsigned region = static_cast<unsigned>(key.back() - '0'); // key came from the catalog
        account.build.armor[region] = it->armor_material;
    }
    if (Shared() && (!target.is_bot || it->armor_material) && !Stat(account.build, key)) team.last_recipient = it->recipient;
    Balance(buyer).held -= it->cost;
    return true;
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
bool ShopOpen() { return run.open && Enabled(); }
bool LoopChosen() { return Enabled() && run.final && run.loop; }
Build GetBuild(ID id) {
    const auto found = run.players.find(id);
    Build b = found == run.players.end() ? Build{} : found->second.build;
    const auto* player = Seat(id);
    if (player && !player->is_bot) {
        const auto team = run.teams.find(ShopTeam(*player));
        if (team != run.teams.end()) {
            b.health += team->second.health;
            b.damage += team->second.damage;
            b.speed += team->second.speed;
            b.stealth_passive += team->second.stealth_passive;
            b.stealth_active += team->second.stealth_active;
        }
    }
    return b;
}
void NewSession() {
    run = Run{};
    spawns.clear();
    last_action.clear();
    next_action = 0;
    dirty = false;
    next_sample = next_send = 0;
    stats_cache.clear();
}
void ForgetPlayer(ID id) {
    if (!Net()->IsHosting() || Net()->LobbyMigrationBusy()) return;
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
    for (auto& p : run.players) p.second.wallet.pooled = 0;
    for (auto& t : run.teams) t.second.wallet.pooled = 0;
    spawns.clear();
    ++run.attempt;
    dirty = true;
}
void NewWorld() {
    spawns.clear();
    stats_cache.clear();
    run.xp_scales.clear();
    next_sample = 0;
    if (!Net()->IsHosting()) return;
    run.open = run.final = run.loop = false;
    run.deadline_ms = 0;
    run.ready.clear();
    ResetAttempt();
}
void Checkpoint() {
    if (!Enabled() || !Net()->IsHosting()) return;
    for (auto& p : run.players) p.second.wallet.Commit();
    for (auto& t : run.teams) t.second.wallet.Commit();
    dirty = true;
}
bool BeginShop(bool final) {
    if (!Enabled() || !Net()->IsHosting() || Net()->LobbyWaiting()) return false;
    if (run.open) return true;
    Checkpoint();
    if (SceneGraph* graph = Engine::Instance()->GetSceneGraph()) SampleXPScales(graph);
    TrainBots();
    run.open = true;
    run.final = final;
    run.loop = false;
    run.ready.clear();
    ++run.shop_generation;
    last_action.clear();
    int humans = 0;
    std::set<int> teams;
    for (const auto& entry : Net()->online_session->player_states) {
        if (!entry.second.is_bot && entry.second.connected) {
            ++humans;
            teams.insert(ShopTeam(entry.second));
        }
    }
    for (int team : teams) Leader(team);
    int index = Net()->ModeSetting("progression_timer");
    int seconds = humans > 1 && index >= 0 && index < 6 ? timers[index] : 0;
    run.deadline_ms = seconds ? OnlineClockMs() + 1000.0 * seconds : 0;
    Publish();
    return true;
}
void Update() {
    if (!Net()->IsHosting() || !Net()->online_session || Net()->LobbyWaiting() || !Enabled()) return;
    const double now = OnlineClockMs();
    if (run.open) {
        bool waiting = false;
        for (const auto& row : Net()->online_session->player_states) {
            if (!row.second.is_bot && row.second.connected && !run.ready.count(row.first)) waiting = true;
        }
        if (!waiting || (run.deadline_ms > 0 && now >= run.deadline_ms)) {
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
    if (!Enabled() || run.open || attacker < 0 || victim == attacker || !std::isfinite(damage) ||
        (!defeat && (!Combat() || damage <= 0)) || !Net()->IsLocalAvatar(victim)) return;
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
bool ReceiveEvent(uint8_t peer, const Json::Value& m) {
    if (!Enabled() || !Net()->IsHosting() || Net()->LobbyWaiting() || run.open || !m["attempt"].isUInt() ||
        m["attempt"].asUInt() != run.attempt || !m["victim"].isInt() || !m["attacker"].isInt() ||
        !m["damage"].isNumeric() || !m["defeat"].isBool()) return false;
    const int victim = Net()->GetObjectID(m["victim"].asInt());
    if (Net()->GetAvatarOwner(victim) != peer) return false;
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
    v["xp_scale"] = run.xp_scales[actor];
    const auto* subject = run.open ? Seat(target) : buyer;
    if (!CanTarget(*buyer,subject)) subject = buyer;
    v["stats"] = PlayerStats(*subject);
    if (!run.open) return v;
    const int team = ShopTeam(*buyer);
    const ID leader = Leader(team);
    v["shared"] = Shared();
    v["team_leader"] = leader;
    v["can_buy"] = !Shared() || actor == leader;
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
    v["owned_dagger"] = bool(account.weapons & Knife);
    v["owned_sword"] = bool(account.weapons & Sword);
    v["owned_spear"] = bool(account.weapons & Spear);
    v["owned_staff"] = bool(account.weapons & Staff);
    v["owned_broadsword"] = bool(account.weapons & Broadsword);
    v["big_weapon"] = account.build.big_weapon;
    v["dagger"] = account.build.dagger;
    v["can_equip"] = subject == buyer || (subject->is_bot && v["can_buy"].asBool());
    v["cards"] = Json::Value(Json::arrayValue);
    if (v["can_buy"].asBool()) for (const auto& offer : Catalog(*buyer,*subject)) {
        Json::Value card;
        if (offer.armor_material) card["armor_material"] = offer.armor_material;
        card["id"] = offer.id; card["label"] = offer.label; card["description"] = offer.description;
        card["rarity"] = offer.rarity; card["cost"] = offer.cost; card["available"] = w.held >= offer.cost && !run.ready.count(actor);
        v["cards"].append(card);
    }
    return v;
}
bool ReceiveAction(ID actor, const Json::Value& m) {
    const auto* buyer = Seat(actor);
    if (!ShopOpen() || !Net()->IsHosting() || !buyer || buyer->is_bot || !buyer->connected ||
        !m["generation"].isUInt() || m["generation"].asUInt() != run.shop_generation ||
        !m["seq"].isUInt() || m["seq"].asUInt() <= last_action[actor] ||
        !m["command"].isString() || !m["key"].isString() || !m["target"].isUInt()) return false;
    last_action[actor] = m["seq"].asUInt();
    const auto action = m["command"].asString(), key = m["key"].asString();
    const ID target = m["target"].asUInt();
    const auto* subject = Seat(target);
    const int team_id = ShopTeam(*buyer);
    const bool may_buy = !Shared() || actor == Leader(team_id);
    if (action == "ready") {
        if (run.ready.count(actor)) run.ready.erase(actor); else run.ready.insert(actor);
    } else if (run.ready.count(actor)) return false;
    else if (action == "loop" && run.final && Net()->IsLobbyHost(actor) && (key == "yes" || key == "no")) run.loop = key == "yes";
    else if (action == "leader" && actor == Leader(team_id) && subject && !subject->is_bot && subject->connected && team_id == ShopTeam(*subject))
        run.teams[team_id].leader = target;
    else if (!CanTarget(*buyer,subject)) return false;
    else if (action == "buy" && may_buy) {
        if (!Buy(*buyer,*subject,key)) return false;
    } else if (action == "equip" && (target == actor || (subject->is_bot && may_buy))) {
        auto& acc = run.players[target];
        if (key != "none" && !(acc.weapons & WeaponFlag(key))) return false;
        if (key == "dagger") acc.build.dagger = !acc.build.dagger;
        else acc.build.big_weapon = WeaponSelection(key);
    } else return false;
    Publish();
    return true;
}
bool Act(ID actor, ID target, const std::string& action, const std::string& key) {
    const auto* owner = Seat(actor);
    if (!ShopOpen() || !owner || owner->peer_id != Net()->LocalPeerID() || owner->is_bot) return false;
    Json::Value m;
    m["action"] = "progress_action"; m["command"] = action; m["key"] = key;
    m["actor"] = actor; m["target"] = target; m["generation"] = run.shop_generation;
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
}
Json::Value Snapshot() {
    Json::Value j;
    j["v"] = 2; j["shop"] = run.shop_generation; j["attempt"] = run.attempt;
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
        r["health"] = b.health; r["damage"] = b.damage; r["speed"] = b.speed;
        r["stealth_passive"] = b.stealth_passive; r["stealth_active"] = b.stealth_active;
        for (int tier : b.armor) r["armor"].append(tier);
        r["weapons"] = a.weapons; r["big"] = b.big_weapon; r["dagger"] = b.dagger;
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
        j["teams"].append(r);
    }
    return j;
}
bool Restore(const Json::Value& j, bool validate_only) {
    if (!j.isObject() || !j["v"].isInt() || (j["v"].asInt() != 1 && j["v"].asInt() != 2) ||
        !j["players"].isArray() || j["players"].size() > 8192 || !j["teams"].isArray() || j["teams"].size() > 8192 ||
        !j["ready"].isArray() || j["ready"].size() > 8192 || !j["shop"].isUInt() || !j["attempt"].isUInt() ||
        !j["open"].isBool() || !j["final"].isBool() || !j["loop"].isBool() || !j["ms_left"].isNumeric()) return false;
    Run next;
    next.shop_generation = j["shop"].asUInt(); next.attempt = j["attempt"].asUInt();
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
        if (!r.isObject() || !r["id"].isUInt() || !r["weapons"].isUInt() || r["weapons"].asUInt() > kWeaponOwnershipMask ||
            !r["dagger"].isBool() || !r["trained"].isBool() || !r["recruited"].isBool() || !r["owner"].isUInt() ||
            !r["armor"].isArray() || r["armor"].size()!=9) return false;
        auto& a = next.players[r["id"].asUInt()]; auto& b = a.build;
        if (!ReadWallet(r["w"],a.wallet) || !ReadInt(r["health"],0,kMaxRank,b.health) ||
            !ReadInt(r["damage"],0,kMaxRank,b.damage) || !ReadInt(r["speed"],0,kMaxRank,b.speed) ||
            !ReadInt(r.get("stealth_passive",0),0,kMaxRank,b.stealth_passive) ||
            !ReadInt(r.get("stealth_active",0),0,kMaxRank,b.stealth_active) ||
            !ReadInt(r["big"],0,kMaxMainWeapon,b.big_weapon) || !ReadInt(r["ai"],0,3,b.ai)) return false;
        for (unsigned i = 0; i < b.armor.size(); ++i) {
            int value;
            if (!ReadInt(r["armor"][i], 0, j["v"].asInt() == 1 ? ArmorProtection::Dog : ArmorProtection::Grey, value)) return false;
            b.armor[i] = ArmorProtection::RestoredShopMaterial(i, value, j["v"].asInt() == 1);
        }
        a.weapons = r["weapons"].asUInt(); b.dagger = r["dagger"].asBool(); b.trained = r["trained"].asBool();
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
            !ReadInt(r.get("stealth_active",0),0,kMaxRank,t.stealth_active)) return false;
        t.leader = r["leader"].asUInt(); t.last_recipient = r["recipient"].asUInt();
    }
    if (!validate_only) run = std::move(next);
    return true;
}
}
