#include <Online/local_profiles.h>
#include <Online/friendly_progression.h>
#include <Online/weapon_categories.h>
#include <Online/online.h>
#include <Online/online_utility.h>
#include <Internal/modloading.h>
#include <Internal/filesystem.h>
#include <Compat/fileio.h>
#include <Logging/logdata.h>
#include <tinyxml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <set>

namespace LocalProfiles {
namespace {
const char* progression = "og-online-progression";
constexpr int roll_cost = 1;
constexpr size_t max_archive = 16 * 1024 * 1024;
using JsonValue = Json::Value;
struct Declaration {
    JsonValue settings{Json::arrayValue};
    std::map<std::string, std::string> subprofiles;
};
std::map<int, Declaration> declarations;
struct Seat {
    std::string profile, guest_name;
    JsonValue temporary, win;
};
std::map<uint32_t, Seat> seats;
std::map<uint32_t, unsigned> rejections;
JsonValue remote, archive;
bool loaded = false, writable = true;
unsigned revision = 1, serial = 0;
std::mt19937& Random() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}
Online* Net() { return Online::Instance(); }
std::string Encode(const JsonValue& value) { return Json::FastWriter().write(value); }
std::string File() { return GetWritePath(CoreGameModID) + "Data/player_profiles.json"; }
bool Commit(JsonValue next);
void Ensure() {
    if (loaded) return;
    loaded = true;
    archive["version"] = 1;
    archive["profiles"] = JsonValue(Json::objectValue);
    archive["settings"] = JsonValue(Json::objectValue);
    FILE* file = my_fopen(File().c_str(), "rb");
    if (!file) return;
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) && data.size() <= max_archive) data.append(buf, n);
    const bool ok = !ferror(file);
    fclose(file);
    JsonValue parsed;
    if (ok && data.size() <= max_archive && Json::Reader().parse(data, parsed, false) &&
        parsed.isObject() && parsed["version"] == 1 && parsed["profiles"].isObject() && parsed["settings"].isObject()) {
        bool migrated = false;
        for (auto& profile : parsed["profiles"]) {
            const auto& old = static_cast<const JsonValue&>(profile)["mods"][progression]["upgrades"]["owned"];
            if (!old.isObject() || !old.isMember("damage")) continue;
            auto& owned = profile["mods"][progression]["upgrades"]["owned"];
            if (owned.removeMember("damage") == true) {
                owned["bone_damage"] = true;
                owned["flesh_damage"] = true;
            }
            migrated = true;
        }
        archive.swap(parsed);
        if (migrated) Commit(archive);
    } else {
        writable = false;
        LOGE << "Unable to read profile archive " << File() << std::endl;
    }
}
bool Commit(JsonValue next) {
    if (!writable) return false;
    const std::string path = File(), tmp = path + ".tmp", data = Encode(next);
    if (data.size() > max_archive) return false;
    CreateParentDirs(path);
    FILE* f = my_fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || movefile(tmp.c_str(), path.c_str()) != 0) {
        deletefile(tmp.c_str());
        return false;
    }
    archive.swap(next);
    ++revision;
    return true;
}
void Refresh() {
    if (Net()->IsHosting()) Net()->BroadcastLobby();
}
const Declaration* Definition(const ModInstance* mod) {
    auto it = declarations.find(mod->GetSid().id);
    return mod->IsActive() && it != declarations.end() ? &it->second : nullptr;
}
const Declaration* Definition(const std::string& id) {
    for (const auto* mod : ModLoading::Instance().GetMods())
        if (mod->id.str() == id && Definition(mod)) return Definition(mod);
    return nullptr;
}
std::string Attr(const TiXmlElement* row, const char* key) {
    const char* val = row->Attribute(key);
    return val ? val : "";
}
bool Token(const std::string& id) {
    return !id.empty() && id.size() <= 64 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
bool NameOK(const std::string& name) {
    return OnlineUtility::IsValidPlayerName(name) && name.front() != ' ' && name.back() != ' ';
}
const JsonValue& Records() {
    Ensure();
    return static_cast<const JsonValue&>(archive)["profiles"];
}
std::string NameById(const std::string& id) {
    if (!id.empty())
        for (auto i = Records().begin(); i != Records().end(); ++i)
            if ((*i)["id"] == id) return i.name();
    return {};
}
using Layer = std::pair<std::string, std::string>;
Layer ActiveLayer() {
    Layer first;
    for (const auto* mod : ModLoading::Instance().GetMods()) {
        const auto* decl = Definition(mod);
        if (!decl) continue;
        for (const auto& sub : decl->subprofiles) {
            if (sub.second.empty() || !GetModSetting(mod->id.str(), sub.second)) continue;
            Layer candidate(mod->id.str(), sub.first);
            if (candidate == Layer(progression, "collection")) return candidate;
            if (first.first.empty() || candidate < first) first = candidate;
        }
    }
    return first;
}
JsonValue& LayerAt(JsonValue& p, const Layer& layer) {
    return layer.first.empty() ? p["base"] : p["mods"][layer.first][layer.second];
}
const JsonValue& LayerAt(const JsonValue& p, const Layer& layer) {
    return layer.first.empty() ? p["base"] : p["mods"][layer.first][layer.second];
}
bool Meta(const Layer& layer) { return layer == Layer(progression, "collection"); }
const Layer wallet_layer{progression, "collection"}, upgrade_layer{progression, "upgrades"};
void MakeRecord(JsonValue& p) {
    p["id"] = NewID();
    p["base"] = JsonValue(Json::objectValue);
    for (const auto* mod : ModLoading::Instance().GetMods())
        if (const auto* decl = Definition(mod))
            for (const auto& sub : decl->subprofiles) p["mods"][mod->id.str()][sub.first] = JsonValue(Json::objectValue);
}
struct OutfitOption {
    const char* race;
    const char* id;
    const char* label;
    const char* thumb;
    int rarity;
};
const OutfitOption outfits[] = {
    {"rabbit", "turner", "Turner", "turner", 0},
    {"rabbit", "civ", "Village clothes", "rabbitcivilian", 0},
    {"rabbit", "guard", "Guard", "rabbitguard", 1},
    {"rabbit", "guard2", "Veteran guard", "rabbitguard2", 2},
    {"rabbit", "pale_turner", "Pale Turner", "pale_turner_actor", 3},
    {"rabbit", "raider_rabbit", "Raider", "raider_rabbit_actor", 4},
    {"rat", "rat", "Field mouse", "rat_actor", 0},
    {"rat", "female_rat", "Scout", "female_rat_actor", 1},
    {"rat", "hooded_rat", "Hooded scout", "Hooded_rat_actor", 3},
    {"cat", "male_cat", "Traveler", "male_cat_actor", 0},
    {"cat", "female_cat", "Skirmisher", "female_cat_actor", 1},
    {"cat", "fancy_striped_cat", "Striped duelist", "fancy_striped_cat_actor", 2},
    {"cat", "ouranian_hand", "Ouranian Hand", "ouranian_hand_actor", 4},
    {"dog", "dog_soldier", "Soldier", "dog", 0},
    {"dog", "lt_dog_male_1", "Light infantry", "light_armored_dog_male_1_actor", 1},
    {"dog", "lt_dog_female", "Light guard", "light_armored_dog_female_actor", 2},
    {"dog", "lt_dog_male_2", "Veteran infantry", "light_armored_dog_male_2_actor", 3},
    {"wolf", "male_wolf", "Timber wolf", "male_wolf_actor", 0},
    {"wolf", "wolf", "Lugaru wolf", "IGF_wolfActor", 4},
};
JsonValue Option(const std::string& id, const std::string& label, const std::string& image, int rarity) {
    JsonValue row;
    row["id"] = id;
    row["label"] = label;
    row["image"] = image;
    row["rarity"] = rarity;
    return row;
}
JsonValue Categories() {
    JsonValue list(Json::arrayValue);
    for (const auto& race : std::vector<std::pair<std::string, std::string>>{
             {"rabbit", "Rabbit"}, {"rat", "Mouse"}, {"cat", "Cat"}, {"dog", "Dog"}, {"wolf", "Wolf"}}) {
        JsonValue row;
        row["id"] = "outfit." + race.first;
        row["label"] = race.second;
        row["kind"] = "outfit";
        for (const auto& o : outfits)
            if (o.race == race.first)
                row["options"].append(Option(o.id, o.label, std::string("Data/UI/spawner/thumbs/Character/") + o.thumb + ".png", o.rarity));
        list.append(row);
    }
    for (const auto& cat : WeaponCategories::Catalog()) {
        if (cat.skins.size() < 2) continue;
        JsonValue row;
        row["id"] = "weapon." + cat.id;
        row["label"] = cat.label;
        row["kind"] = "weapon";
        for (const auto& skin : cat.skins) row["options"].append(Option(skin.id, skin.label, skin.artwork, skin.rarity));
        list.append(row);
    }
    return list;
}
bool Allowed(const JsonValue& layer, const JsonValue& cat, const std::string& option, bool meta) {
    for (const auto& opt : cat["options"])
        if (opt["id"] == option)
            return !meta || cat["options"][0]["id"] == option || layer["owned"][cat["id"].asString()][option].asBool();
    return false;
}
JsonValue Profile(const std::string& name, const JsonValue& state, bool meta) {
    JsonValue out;
    out["name"] = name;
    out["guest"] = name.empty();
    out["tokens"] = state.get("tokens", 0u);
    out["meta"] = meta;
    out["selected"] = state["selected"].isObject() ? state["selected"] : JsonValue(Json::objectValue);
    out["unlocked"] = JsonValue(Json::objectValue);
    for (auto c = state["owned"].begin(); c != state["owned"].end(); ++c)
        for (auto o = c->begin(); o != c->end(); ++o)
            if (o->asBool()) out["unlocked"][c.name()].append(o.name());
    return out;
}
JsonValue Names() {
    JsonValue names(Json::arrayValue);
    for (auto it = Records().begin(); it != Records().end(); ++it) names.append(it.name());
    return names;
}
std::string GuestName() {
    static const char* choices[] = {"Turner", "Drika", "Cobus", "Ferdi", "Jairo", "Gunk", "Bijou", "Donatas"};
    const unsigned start = Random()() % 8;
    std::set<std::string> used;
    for (const auto& p : Net()->GetPlayerStates()) used.insert(p.playername);
    for (unsigned i = 0;; ++i) {
        std::string name = choices[(start + i) % 8];
        if (i >= 8) name += " " + std::to_string(1 + i / 8);
        if (!used.count(name) && !Records().isMember(name)) return name;
    }
}
struct Prize {
    std::string category, option;
    int weight;
};
std::vector<Prize> Prizes(const JsonValue& state) {
    constexpr int weights[] = {32, 16, 8, 3, 1};
    std::vector<Prize> pool;
    for (const auto& cat : Categories())
        for (const auto& o : cat["options"]) {
            const auto id = o["id"].asString();
            if (!Allowed(state, cat, id, true))
                pool.push_back({cat["id"].asString(), id, weights[std::max(0, std::min(4, o["rarity"].asInt()))]});
        }
    return pool;
}
// Draw the rarity first, then a remaining entry within that rarity. Adding a
// weapon pack therefore cannot drown out the common stat upgrades.
using UpgradeTiers = std::array<std::vector<FriendlyProgression::UpgradeDefinition>, 5>;
UpgradeTiers UpgradePrizes(const JsonValue& record) {
    UpgradeTiers tiers;
    const auto& owned = LayerAt(record, upgrade_layer)["owned"];
    for (const auto& upgrade : FriendlyProgression::UpgradeCatalog())
        if (!owned.get(upgrade.id, false).asBool() &&
            (upgrade.prerequisite.empty() || owned.get(upgrade.prerequisite, false).asBool()))
            tiers.at(upgrade.rarity).push_back(upgrade);
    return tiers;
}
void RollControls(JsonValue& view, const JsonValue& record, bool upgrades) {
    const auto& wallet = LayerAt(record, wallet_layer);
    view["tokens"] = wallet.get("tokens", 0u);
    view["roll_cost"] = roll_cost;
    view["upgrade_progression"] = upgrades;
    const bool funded = view["tokens"].asUInt() >= roll_cost;
    view["can_roll"] = funded && view["meta"].asBool() && !Prizes(wallet).empty();
    if (funded && upgrades) {
        const auto tiers = UpgradePrizes(record);
        view["can_roll_upgrade"] = std::any_of(tiers.begin(), tiers.end(), [](const auto& t) { return !t.empty(); });
    } else view["can_roll_upgrade"] = false;
}

bool SpendRoll(const std::string& name, Seat* seat, bool upgrade, bool upgrades_enabled) {
    if (upgrade ? !upgrades_enabled : !Meta(ActiveLayer())) return false;
    if (name.empty() ? seat == nullptr : !Records().isMember(name)) return false;
    JsonValue record = name.empty() ? seat->temporary : Records()[name];
    JsonValue win;
    auto& wallet = LayerAt(record, wallet_layer);
    const auto tokens = wallet.get("tokens", 0u).asUInt();
    if (tokens < roll_cost) return false;
    if (upgrade) {
        const auto pool = UpgradePrizes(record);
        std::array<int, 5> weights{65536, 512, 64, 8, 1};
        for (size_t i = 0; i < weights.size(); ++i)
            if (pool[i].empty()) weights[i] = 0;
        if (std::none_of(weights.begin(), weights.end(), [](int weight) { return weight > 0; })) return false;
        const auto& tier = pool[std::discrete_distribution<size_t>(weights.begin(), weights.end())(Random())];
        const auto& prize = tier[std::uniform_int_distribution<size_t>(0, tier.size() - 1)(Random())];
        LayerAt(record, upgrade_layer)["owned"][prize.id] = true;
        win = Option(prize.id, prize.label, prize.artwork, prize.rarity);
        win["kind"] = "upgrade";
    } else {
        const auto prizes = Prizes(wallet);
        if (prizes.empty()) return false;
        std::vector<int> weights;
        for (const auto& p : prizes) weights.push_back(p.weight);
        const auto& p = prizes[std::discrete_distribution<size_t>(weights.begin(), weights.end())(Random())];
        wallet["owned"][p.category][p.option] = true;
        for (const auto& cat : Categories()) if (cat["id"] == p.category)
            for (const auto& option : cat["options"]) if (option["id"] == p.option) win = option;
        win["kind"] = "cosmetic";
        win["category"] = p.category;
        win["option"] = p.option;
    }
    wallet["tokens"] = tokens - roll_cost;
    // Keep the latest prize with its profile. The settings screen can show
    // the result even when this name has no active lobby seat.
    wallet["last_roll"] = win;
    if (name.empty()) {
        seat->temporary = std::move(record);
    } else {
        JsonValue next = archive;
        next["profiles"][name] = std::move(record);
        if (!Commit(std::move(next))) return false;
    }
    win["serial"] = ++serial;
    if (seat) seat->win = win;
    else if (Net()->IsHosting()) {
        const auto profile = Records()[name]["id"].asString();
        for (auto& entry : seats) if (entry.second.profile == profile) entry.second.win = win;
    }
    ++revision;
    Refresh();
    return true;
}
}  // namespace
std::string NewID() {
    static unsigned counter = 0;
    return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter);
}
unsigned Revision() { return revision; }
void ModsChanged() { ++revision; }
bool HasPrimary() {
    Ensure();
    return !archive.get("primary", "").asString().empty();
}
void UsePrimary(uint32_t slot) {
    Ensure();
    const auto id = archive.get("primary", "").asString();
    if (!NameById(id).empty()) seats[slot].profile = id;
}
bool RegisterMod(int sid, const TiXmlElement* settings, const TiXmlElement* profiles) {
    declarations.erase(sid);
    ++revision;
    if (!settings && !profiles) return true;
    Declaration decl;
    std::set<std::string> keys;
    for (auto* s = settings ? settings->FirstChildElement("Setting") : nullptr; s; s = s->NextSiblingElement("Setting")) {
        const auto key = Attr(s, "key"), label = Attr(s, "label"), type = Attr(s, "type");
        if (!Token(key) || label.empty() || !keys.insert(key).second || (type != "checkbox" && type != "choice")) return false;
        JsonValue row;
        row["id"] = key;
        row["label"] = label;
        row["type"] = type;
        row["options"] = JsonValue(Json::arrayValue);
        for (auto* ch = s->FirstChildElement("Choice"); ch; ch = ch->NextSiblingElement("Choice")) {
            if (!ch->GetText()) return false;
            row["options"].append(ch->GetText());
        }
        int val = 0;
        if (s->Attribute("default") && s->QueryIntAttribute("default", &val) != TIXML_SUCCESS) return false;
        const int size = type == "checkbox" ? 2 : static_cast<int>(row["options"].size());
        if (val < 0 || val >= size || size > 128) return false;
        row["default"] = val;
        decl.settings.append(row);
    }
    for (auto* sub = profiles ? profiles->FirstChildElement("SubProfile") : nullptr; sub; sub = sub->NextSiblingElement("SubProfile")) {
        const auto key = Attr(sub, "id"), toggle = Attr(sub, "active_when");
        if (!Token(key) || (!toggle.empty() && !keys.count(toggle)) || !decl.subprofiles.emplace(key, toggle).second) return false;
    }
    declarations.emplace(sid, std::move(decl));
    return true;
}
int GetModSetting(const std::string& mod, const std::string& key) {
    Ensure();
    if (const auto* d = Definition(mod))
        for (const auto& setting : d->settings)
            if (setting["id"] == key) {
                const auto& value = static_cast<const JsonValue&>(archive)["settings"][mod][key];
                const int count = setting["type"] == "checkbox" ? 2 : static_cast<int>(setting["options"].size());
                return value.isInt() && value.asInt() >= 0 && value.asInt() < count ? value.asInt() : setting["default"].asInt();
            }
    return 0;
}
bool SetModSetting(const std::string& mod, const std::string& key, int value) {
    Ensure();
    if (const auto* d = Definition(mod))
        for (const auto& setting : d->settings)
            if (setting["id"] == key) {
                int size = setting["type"] == "checkbox" ? 2 : static_cast<int>(setting["options"].size());
                if (value < 0 || value >= size) return false;
                JsonValue next = archive;
                next["settings"][mod][key] = value;
                if (!Commit(std::move(next))) return false;
                Refresh();
                return true;
            }
    return false;
}
std::string SettingsView() {
    JsonValue out;
    out["names"] = Names();
    out["primary"] = NameById(archive.get("primary", "").asString());
    out["categories"] = Categories();
    out["mods"] = JsonValue(Json::arrayValue);
    for (const auto* mod : ModLoading::Instance().GetMods())
        if (const auto* decl = Definition(mod)) {
            if (decl->settings.empty()) continue;
            JsonValue tab;
            tab["id"] = mod->id.str();
            tab["label"] = mod->name.str();
            tab["settings"] = decl->settings;
            for (auto& setting : tab["settings"]) setting["value"] = GetModSetting(mod->id.str(), setting["id"].asString());
            out["mods"].append(tab);
        }
    return Encode(out);
}
std::string View(const std::string& name) {
    const auto layer = ActiveLayer();
    const auto& record = Records()[name];
    auto view = Profile(name, LayerAt(record, layer), Meta(layer));
    view["upgrades"] = LayerAt(record, upgrade_layer)["owned"];
    RollControls(view, record, GetModSetting(progression, "upgrade_progression") != 0);
    view["win"] = LayerAt(record, wallet_layer)["last_roll"];
    return Encode(view);
}
bool Edit(const std::string& action, const std::string& name, const std::string& key, const std::string& value) {
    Ensure();
    if (action == "primary") {
        if (!name.empty() && !Records().isMember(name)) return false;
        JsonValue prefs = archive;
        prefs["primary"] = name.empty() ? "" : Records()[name]["id"].asString();
        return Commit(std::move(prefs));
    }
    if (!NameOK(name)) return false;
    if (action == "roll" || action == "roll_upgrade")
        return SpendRoll(name, nullptr, action == "roll_upgrade", GetModSetting(progression, "upgrade_progression") != 0);
    JsonValue next = archive;
    auto& rows = next["profiles"];
    if (action == "create") {
        if (rows.isMember(name)) return false;
        MakeRecord(rows[name]);
    } else {
        if (!rows.isMember(name)) return false;
        if (action == "delete") {
            if (next["primary"] == rows[name]["id"]) next.removeMember("primary");
            rows.removeMember(name);
        } else if (action == "rename") {
            if (!NameOK(value) || rows.isMember(value)) return false;
            rows[value] = rows.removeMember(name);
        } else if (action == "set") {
            const auto layer = ActiveLayer();
            auto& data = LayerAt(rows[name], layer);
            bool allowed = false;
            for (const auto& cat : Categories())
                if (cat["id"] == key) allowed = Allowed(data, cat, value, Meta(layer));
            if (!allowed) return false;
            data["selected"][key] = value;
        } else
            return false;
    }
    if (!Commit(std::move(next))) return false;
    Refresh();
    return true;
}
std::string GetData(const std::string& name, const std::string& mod, const std::string& sub) {
    const auto* d = Definition(mod);
    if (!d || !d->subprofiles.count(sub) || !Records().isMember(name)) return {};
    return Encode(Records()[name]["mods"][mod][sub]["data"]);
}
bool SetData(const std::string& name, const std::string& mod, const std::string& sub, const std::string& json) {
    const auto* d = Definition(mod);
    if (!d || !d->subprofiles.count(sub) || !Records().isMember(name) || json.size() > 65536) return false;
    JsonValue val;
    if (!Json::Reader().parse(json, val, false)) return false;
    JsonValue next = archive;
    next["profiles"][name]["mods"][mod][sub]["data"] = val;
    return Commit(std::move(next));
}
void ResetLobby() {
    seats.clear();
    rejections.clear();
    remote = JsonValue();
    ++revision;
}
void SyncRoster() {
    for (auto& entry : Net()->online_session->player_states) {
        if (entry.second.is_bot) {
            seats.erase(entry.first);
            continue;
        }
        auto& seat = seats[entry.first];
        const auto name = NameById(seat.profile);
        if (!name.empty()) {
            entry.second.playername = name;
            continue;
        }
        if (!seat.profile.empty()) {
            seat.profile.clear();
            seat.guest_name.clear();
            seat.temporary.clear();
            seat.win.clear();
        }
        if (seat.guest_name.empty()) seat.guest_name = GuestName();
        entry.second.playername = seat.guest_name;
    }
    for (auto it = seats.begin(); it != seats.end();) {
        if (!Net()->GetLobbySlot(it->first) && !Net()->online_session->player_states.count(it->first))
            it = seats.erase(it);
        else
            ++it;
    }
}
void SetGuestName(uint32_t id, const std::string& name) {
    if (seats[id].profile.empty()) seats[id].guest_name = name;
}
void Swap(uint32_t first, uint32_t second) {
    std::swap(seats[first], seats[second]);
    ++revision;
}
void SaveSlots(JsonValue& slots) {
    for (auto& row : slots["slots"]) {
        auto it = seats.find(row["id"].asUInt());
        if (it != seats.end() && !it->second.profile.empty()) row["profile"] = it->second.profile;
    }
}
void LoadSlots(const JsonValue& slots) {
    seats.clear();
    for (const auto& row : slots["slots"]) {
        if (row["kind"] != "player") continue;
        auto& seat = seats[row["id"].asUInt()];
        seat.profile = row.get("profile", "").asString();
        seat.guest_name = row["name"].asString();
    }
    ++revision;
}
JsonValue PublicState() {
    // Publish only choices needed to draw each connected player. Mod-owned
    // data, inactive subprofiles and the rest of the archive stay on this host.
    const auto layer = ActiveLayer();
    JsonValue state;
    if (Net()->LobbyWaiting()) state["names"] = Names();
    state["meta"] = Meta(layer);
    const bool upgrades = Net()->LobbyWaiting() && GetModSetting(progression, "upgrade_progression") != 0;
    state["players"] = JsonValue(Json::objectValue);
    for (const auto& entry : seats) {
        if (!Net()->online_session->player_states.count(entry.first)) continue;
        const auto& seat = entry.second;
        const auto name = NameById(seat.profile);
        const auto& record = name.empty() ? seat.temporary : Records()[name];
        const auto& data = LayerAt(record, layer);
        auto& row = state["players"][std::to_string(entry.first)];
        row = Profile(name, data, Meta(layer));
        if (Net()->LobbyWaiting()) {
            RollControls(row, record, upgrades);
            row["win"] = seat.win;
        } else {
            row.removeMember("unlocked");
            row.removeMember("tokens");
        }
    }
    Receive(state);
    return state;
}
void Rejected(uint32_t seat) {
    rejections[seat] = ++serial;
    ++revision;
}
void Receive(const JsonValue& state) {
    if (state != remote) {
        remote = state;
        ++revision;
    }
}
bool CanEdit(uint32_t id, uint32_t actor) {
    Online* net = Net();
    if (!net->LobbyWaiting() || !net->online_session || net->LobbyMigrationBusy()) return false;
    const auto& players = net->online_session->player_states;
    auto target = players.find(id), owner = players.find(actor);
    return target != players.end() && owner != players.end() && !target->second.is_bot && target->second.connected &&
           !owner->second.is_bot && owner->second.connected &&
           (target->second.peer_id == owner->second.peer_id && (net->IsLobbyLeader(actor) || net->IsLobbyHost(actor)));
}
std::string LobbyView(uint32_t id, uint32_t actor) {
    const JsonValue& data = remote;
    JsonValue out = data["players"][std::to_string(id)];
    const auto error = rejections.find(id);
    if (error != rejections.end()) {
        out["error"]["serial"] = error->second;
        out["error"]["message"] = "Profile change declined. Check the name, availability and unlocks.";
    }
    out["names"] = data["names"];
    out["categories"] = Categories();
    out["can_edit"] = CanEdit(id, actor);
    return Encode(out);
}
bool Change(uint32_t id, const std::string& action, const std::string& key, const std::string& value) {
    if (!Net()->IsHosting() || !Net()->LobbyWaiting()) return false;
    auto& seat = seats[id];
    if (action == "select" || action == "create") {
        if (action == "select" && value.empty() && seat.profile.empty()) return true;
        if (!value.empty()) {
            if (action == "create" && !Edit("create", value, "", "")) return false;
            if (!Records().isMember(value)) return false;
            const auto selected = Records()[value]["id"].asString();
            for (const auto& other : seats)
                if (other.first != id && other.second.profile == selected && Net()->online_session->player_states.count(other.first)) return false;
            // A saved or disconnected slot can reserve this profile. Once a
            // new occupant claims it, a later reconnect must come back as a guest.
            for (auto& other : seats) {
                if (other.first != id && other.second.profile == selected) other.second = Seat{};
            }
            seat.profile = selected;
        } else if (action == "create")
            return false;
        else if (!seat.profile.empty())
            seat.guest_name.clear();
        if (!value.empty() || !seat.profile.empty()) seat.temporary = JsonValue();
        if (value.empty()) seat.profile.clear();
        seat.win = JsonValue();
    } else {
        const auto name = NameById(seat.profile);
        if (action == "set" && !name.empty()) return Edit("set", name, key, value);
        const auto layer = ActiveLayer();
        if (action == "set") {
            bool allowed = false;
            for (const auto& cat : Categories())
                if (cat["id"] == key) allowed = Allowed(LayerAt(seat.temporary, layer), cat, value, Meta(layer));
            if (!allowed) return false;
            LayerAt(seat.temporary, layer)["selected"][key] = value;
        } else if (action == "roll" || action == "roll_upgrade") {
            return SpendRoll(name, &seat, action == "roll_upgrade", GetModSetting(progression, "upgrade_progression") != 0);
        } else
            return false;
    }
    ++revision;
    Refresh();
    return true;
}
std::set<std::string> UpgradePool(bool& enabled) {
    enabled = GetModSetting(progression, "upgrade_progression") != 0;
    std::set<std::string> pool;
    if (!enabled || !Net()->IsHosting()) return pool;
    for (const auto& player : Net()->GetPlayerStates()) {
        if (player.is_bot || !player.connected) continue;
        const auto it = seats.find(player.player_id);
        if (it == seats.end()) continue;
        const auto name = NameById(it->second.profile);
        const auto& record = name.empty() ? it->second.temporary : Records()[name];
        const auto& owned = LayerAt(record, upgrade_layer)["owned"];
        for (auto upgrade = owned.begin(); upgrade != owned.end(); ++upgrade)
            if (upgrade->asBool()) pool.insert(upgrade.name());
    }
    return pool;
}
std::string WeaponSkin(uint32_t slot, const std::string& category) {
    const JsonValue& current = remote;
    return current["players"][std::to_string(slot)]["selected"].get("weapon." + category, "").asString();
}
std::string Outfit(uint32_t slot, const std::string& species) {
    const JsonValue& player = static_cast<const JsonValue&>(remote)["players"][std::to_string(slot)];
    if (!player.isObject()) return {};
    const std::string chosen = player["selected"].get("outfit." + species, "").asString();
    if (chosen.empty() && player.get("guest", true).asBool()) return {};
    for (const auto& o : outfits)
        if (species == o.race && (chosen.empty() || chosen == o.id)) return std::string("Data/Characters/") + o.id + ".xml";
    return {};
}
void Award(ClearKind kind, const std::string& run, unsigned completion, const std::vector<int>* winners) {
    if (!Net()->IsHosting() || run.empty() || !completion ||
        (!GetModSetting(progression, "upgrade_progression") && !GetModSetting(progression, "meta_progression"))) return;
    constexpr unsigned clears_per_token = 10;
    const char* counter = nullptr;
    switch (kind) {
        case ClearKind::CampaignLevel: counter = "campaign_levels"; break;
        case ClearKind::ArenaMatch: counter = "arena_matches"; break;
        case ClearKind::SurvivalBattery: counter = "survival_batteries"; break;
    }
    Ensure();
    JsonValue next = archive;
    bool changed = false;
    for (const auto& player : Net()->GetPlayerStates()) {
        if (player.is_bot || !player.connected || (winners && std::find(winners->begin(), winners->end(), player.object_id) == winners->end())) continue;
        auto it = seats.find(player.player_id);
        if (it == seats.end()) continue;
        const auto name = NameById(it->second.profile);
        auto& state = LayerAt(name.empty() ? it->second.temporary : next["profiles"][name], wallet_layer);
        if (state["clears"].get(run, 0u).asUInt() >= completion) continue;
        state["clears"][run] = completion;
        // Older profiles have only a wallet and per-run replay watermarks;
        // those marks include ordinary rounds and cannot reconstruct mode totals.
        auto& totals = state["clear_counts"];
        const auto previous = totals.get(counter, 0u).asUInt64();
        if (previous < UINT64_MAX) {
            totals[counter] = previous + 1;
            const unsigned tokens = state.get("tokens", 0u).asUInt();
            if ((previous + 1) % clears_per_token == 0 && tokens < UINT32_MAX)
                state["tokens"] = tokens + 1;
        }
        if (!name.empty()) changed = true;
    }
    if (changed) Commit(std::move(next));
}
}  // namespace LocalProfiles
