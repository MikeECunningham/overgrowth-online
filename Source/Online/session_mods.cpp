#include <Online/session_mods.h>
#include <Internal/modloading.h>
#include <Internal/settings_changes.h>
#include <Main/engine.h>
#include <Online/Message/lobby_control.h>
#include <Online/friendly_progression.h>
#include <Online/online.h>

#include <map>

namespace SessionMods {
namespace {
constexpr size_t kMaxScriptBytes = 64 * 1024;
constexpr size_t kMaxTotalScriptBytes = 256 * 1024;
std::map<std::string, Json::Value> script_data;

size_t DataBytes(const Json::Value& value) {
    return value.empty() ? 0 : Json::FastWriter().write(value).size();
}

ModInstance* ActiveMod(const std::string& id) {
    for (auto* mod : ModLoading::Instance().GetMods())
        if (mod->IsActive() && mod->id.str() == id) return mod;
    return nullptr;
}
const std::string& Version(const ModInstance& mod, Scope scope) {
    if (scope == Scope::Handoff && !mod.SessionVersion(true).empty()) {
        static const std::string omitted;
        return omitted;  // Already carried in the ordinary network snapshot.
    }
    return mod.SessionVersion(scope == Scope::Network);
}
Json::Value Section(const ModInstance& mod, Scope scope) {
    Json::Value section;
    section["version"] = Version(mod, scope);
    const auto found = script_data.find(mod.id.str());
    section["data"] = found == script_data.end() ? Json::Value(Json::objectValue) : found->second;
    if (mod.session.feature == "progression") section["runtime"] = FriendlyProgression::Snapshot();
    if (mod.session.feature == "scaling")
        Json::Reader().parse(Online::Instance()->ModeChallengePayload(), section["runtime"], false);
    return section;
}

bool Runtime(const ModInstance& mod, const Json::Value& section, bool validate_only) {
    if (mod.session.feature == "scaling")
        return Online::Instance()->ReceiveModeChallenge(Json::FastWriter().write(section["runtime"]), validate_only);
    if (mod.session.feature == "progression")
        return FriendlyProgression::Restore(section["runtime"], validate_only);
    return !section.isMember("runtime");
}

bool Check(const Json::Value& mods, Scope scope, bool partial, std::string& error) {
    if (!mods.isObject()) {
        error = "The session mod list is invalid.";
        return false;
    }
    size_t data_bytes = 0;
    for (const auto& id : mods.getMemberNames()) {
        const auto& section = mods[id];
        const auto* mod = ActiveMod(id);
        if (!mod || Version(*mod, scope).empty()) {
            error = "Enable the required mod " + id + " to load this session.";
            return false;
        }
        if (!section.isObject() || !section["version"].isString() || section["version"].asString() != Version(*mod, scope)) {
            error = "Incompatible " + std::string(scope == Scope::Network ? "network" : "save") + " version for mod " + id + ".";
            return false;
        }
        const auto& data = section["data"];
        const size_t bytes = DataBytes(data);
        data_bytes += bytes;
        if (!data.isObject() || bytes > kMaxScriptBytes || data_bytes > kMaxTotalScriptBytes || !Runtime(*mod, section, true)) {
            error = "Invalid session data for mod " + id + ".";
            return false;
        }
    }
    if (scope == Scope::Network && !partial) {
        for (const auto* mod : ModLoading::Instance().GetMods())
            if (mod->IsActive() && !Version(*mod, scope).empty() && !mods.isMember(mod->id.str())) {
                error = "The host is missing mod " + mod->id.str() + ".";
                return false;
            }
    }
    return true;
}
void Apply(const Json::Value& mods) {
    for (const auto& id : mods.getMemberNames()) {
        const auto& section = mods[id];
        if (const auto* mod = ActiveMod(id)) {
            script_data[id] = section["data"];
            Runtime(*mod, section, false);
        }
    }
}
void Publish(const ModInstance& mod) {
    if (!Online::Instance()->IsHosting() || mod.session.network_version.empty()) return;
    Json::Value message;
    message["action"] = "mod_state";
    message["session_mods"][mod.id.str()] = Section(mod, Scope::Network);
    Online::Instance()->Send<OnlineMessages::LobbyControl>(Json::FastWriter().write(message));
}
}

Json::Value Snapshot(Scope scope) {
    Json::Value mods(Json::objectValue);
    for (const auto* mod : ModLoading::Instance().GetMods())
        if (mod->IsActive() && !Version(*mod, scope).empty() &&
            (scope != Scope::Save || mod->session.feature != "survival" || Online::Instance()->IsSurvivalMode()))
            mods[mod->id.str()] = Section(*mod, scope);
    return mods;
}
bool Validate(const Json::Value& mods, Scope scope, std::string& error) {
    return Check(mods, scope, false, error);
}
void Restore(const Json::Value& mods, Scope scope) {
    if (scope == Scope::Save) {
        // Extra installed mods start fresh when opening a card made without them.
        script_data.clear();
        for (const auto* mod : ModLoading::Instance().GetMods()) {
            if (!mod->IsActive() || mods.isMember(mod->id.str())) continue;
            if (mod->session.feature == "progression") FriendlyProgression::NewSession();
            if (mod->session.feature == "scaling") Online::Instance()->ResetModeChallenge();
        }
    }
    Apply(mods);
}
// Character deployment resets native XP attempts and freezes enemy stats.
// Scripts already received their saved JSON before level initialization. Keep
// any changes their lobby or level hooks have made since.
void RestoreGameplay(const Json::Value& save) {
    const auto& mods = save["session_mods"];
    if (!mods.isMember("og-online-progression"))
        FriendlyProgression::SetSavedProfileRun("save-" + save["image_id"].asString());
    for (const auto& id : mods.getMemberNames())
        if (const auto* mod = ActiveMod(id)) Runtime(*mod, mods[id], false);
}
bool ReceiveUpdate(const Json::Value& mods) {
    std::string error;
    if (!mods.isObject() || mods.size() != 1 || !Check(mods, Scope::Network, true, error)) return false;
    Apply(mods);
    return true;
}
void PublishFeature(const std::string& feature) {
    for (const auto* mod : ModLoading::Instance().GetMods())
        if (mod->IsActive() && mod->session.feature == feature) Publish(*mod);
}
void Clear() { script_data.clear(); }

std::string GetData(const std::string& id) {
    auto* mod = ActiveMod(id);
    if (!mod || (mod->session.network_version.empty() && mod->session.save_version.empty())) return "";
    const auto found = script_data.find(id);
    if (found == script_data.end()) return "{}";
    std::string json = Json::FastWriter().write(found->second);
    json.pop_back();  // FastWriter terminates its document with a newline.
    return json;
}
bool SetData(const std::string& id, const std::string& json) {
    auto* online = Online::Instance();
    if (!online->IsHosting() && !(online->CanAdoptLocalSession() && Engine::Instance()->IsLevelLoaded())) return false;
    const auto* mod = ActiveMod(id);
    if (!mod || (mod->session.network_version.empty() && mod->session.save_version.empty()) || json.size() > kMaxScriptBytes) return false;
    Json::Value data;
    if (!Json::Reader().parse(json, data, false) || !data.isObject()) return false;
    size_t bytes = DataBytes(data);
    if (bytes > kMaxScriptBytes) return false;
    for (const auto& entry : script_data)
        if (entry.first != id) bytes += DataBytes(entry.second);
    if (bytes > kMaxTotalScriptBytes) return false;
    const auto previous = script_data.find(id);
    if (previous != script_data.end() && previous->second == data) return true;
    script_data[id] = std::move(data);
    Publish(*mod);
    return true;
}

// Version 1 predated mod descriptors. Its two built-in providers used these
// fixed keys and schema 1; using the installed schema here would hide incompatibility.
static void MigrateLegacySave(Json::Value& save) {
    auto& mods = save["session_mods"] = Json::Value(Json::objectValue);
    auto& rules = save["rules"];
    Json::Value challenge = rules.removeMember("challenge");
    rules["mode_settings"] = challenge.removeMember("modes");
    auto import = [&](const char* id, const Json::Value& runtime) {
        auto& section = mods[id];
        section["version"] = "1";
        section["data"] = Json::Value(Json::objectValue);
        section["runtime"] = runtime;
    };
    Json::Value ledger = save.removeMember("progression");
    bool scaling_used = !Online::Instance()->ReceiveModeChallenge(Json::FastWriter().write(challenge), true);
    if (!scaling_used && challenge.isMember("pressure")) {
        const auto& enemy = challenge["enemy_difficulty"];
        scaling_used = challenge["pressure"].asDouble() != 0 || challenge["seconds"].asDouble() != 0 ||
                       (challenge["loop"].isUInt() && challenge["loop"].asUInt() != 0) ||
                       !challenge["completions"].empty() || !challenge["encounters"].empty() ||
                       !challenge["wipes"].empty() || !challenge["foes"].empty() ||
                       (enemy.isObject() && (enemy["stats"].asBool() || enemy["ai"].asBool() ||
                                             enemy["armor"].asBool() || enemy["weapons"].asBool()));
    }
    bool progression_used = !FriendlyProgression::Restore(ledger, true) || !ledger["players"].empty() ||
                            !ledger["teams"].empty() || ledger["open"] != false || ledger["loop"] != false ||
                            ledger["final"] != false || ledger["shop"].asUInt() != 0;
    const bool have_scaling = ActiveMod("og-online-scaling") != nullptr;
    const bool have_progression = ActiveMod("og-online-progression") != nullptr;
    // Older cards always wrote both modules, including their pristine defaults
    // in offline games. Only require a missing module when the card used it.
    auto& settings = rules["mode_settings"];
    if (settings.isObject()) for (const auto& mode : settings.getMemberNames()) {
        auto& prefs = settings[mode];
        if (!prefs.isObject()) continue;
        for (const auto& key : prefs.getMemberNames()) {
            const bool scaling = key.compare(0, 10, "challenge_") == 0;
            const bool progression = key.compare(0, 11, "progression") == 0;
            const int inherited = scaling && key != "challenge_performance" ? 2 : 0;
            if (prefs[key] != inherited) {
                scaling_used |= scaling;
                progression_used |= progression;
            } else if ((scaling && !have_scaling) || (progression && !have_progression)) prefs.removeMember(key);
        }
    }
    auto& match = rules["match"];
    if (match.isObject()) for (const auto& key : match.getMemberNames()) {
        if (!SettingsChanges::ScalingMatchSetting(key)) continue;
        const auto& v = match[key];
        const bool inherited = key == "difficulty_scaling" ? (v == "0" || v == "false") :
                               key == "campaign_ai_scaling_rate" ? v == "2" : (v == "1" || v == "true");
        if (!inherited) scaling_used = true;
        else if (!have_scaling) match.removeMember(key);
    }
    if (have_scaling || scaling_used) import("og-online-scaling", challenge);
    if (have_progression || progression_used) import("og-online-progression", ledger);
    save["version"] = 2;
}

void MigrateSave(Json::Value& save) {
    if (save["version"] == 1) MigrateLegacySave(save);
    const char* id = "og-online-progression";
    auto& mods = save["session_mods"];
    if (!mods.isObject() || !mods.isMember(id) || !mods[id].isObject()) return;
    auto& runtime = mods[id]["runtime"];
    if (runtime.isObject()) {
        const auto& old = static_cast<const Json::Value&>(runtime);
        const bool missing = !old.isMember("profile_run") && !old.isMember("profile_event") && !old.isMember("profile_completion");
        const bool empty = old["profile_run"] == "" && old["profile_event"] == "" &&
                           old["profile_completion"].isUInt() && old["profile_completion"].asUInt() == 0;
        if (missing || empty) {
            // Pre-profile cards have no run identity. Reopening the same card
            // must reproduce its receipts, including before its first milestone.
            runtime["profile_run"] = "save-" + save["image_id"].asString();
            runtime["profile_event"] = "";
            runtime["profile_completion"] = 0u;
        }
    }
    // Numeric weapon unlocks in earlier saves map to their categories during
    // restore. Pending hands keep their first eligible category in draw order.
    if (mods[id]["version"] == "2" || mods[id]["version"] == "3" || mods[id]["version"] == "4" ||
        mods[id]["version"] == "5" || mods[id]["version"] == "6" || mods[id]["version"] == "7") {
        mods[id]["version"] = "8";
        return;
    }
    if (mods[id]["version"] != "1") return;
    auto& rules = save["rules"];
    if (!rules.isObject() || !rules["mode_settings"].isObject()) return;
    auto& settings = rules["mode_settings"];
    for (const auto& mode : settings.getMemberNames()) {
        auto& prefs = settings[mode];
        if (!prefs.isObject() || !prefs.isMember("progression_combat")) continue;
        const auto& combat = prefs["progression_combat"];
        // Leave malformed values for the normal save validator to reject.
        if (!combat.isInt() || (combat.asInt() != 0 && combat.asInt() != 1)) continue;
        if (mode == "com-wolfire-arena") {
            prefs["progression_gain_kill"] = 1 - combat.asInt();
            prefs["progression_gain_damage"] = combat.asInt();
            prefs["progression_loss_damage"] = combat.asInt();
        }
        prefs.removeMember("progression_combat");
    }
    mods[id]["version"] = "8";
}

void KeepLobbyEdits(Json::Value& mods) {
    for (const auto& id : mods.getMemberNames()) {
        const auto* mod = ActiveMod(id);
        if (!mod) continue;
        // Roster edits can remove a slot or transfer its team while the host
        // is preparing to resume. Preserve the remaining shop timer and state.
        if (mod->session.feature == "progression") {
            auto& runtime = mods[id]["runtime"];
            const auto saved_timer = runtime["ms_left"];
            runtime = FriendlyProgression::Snapshot();
            runtime["ms_left"] = saved_timer;
        }
    }
}
void CloseLegacyRoundShop(Json::Value& mods) {
    for (const auto& id : mods.getMemberNames()) {
        const auto* mod = ActiveMod(id);
        if (mod && mod->session.feature == "progression") {
            auto& ledger = mods[id]["runtime"];
            ledger["open"] = false;
            ledger["ms_left"] = -1.0;
            ledger["ready"] = Json::Value(Json::arrayValue);
        }
    }
}
}
