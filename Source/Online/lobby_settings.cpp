// Host-owned per-mode settings declared by active mods. Apache-2.0.
#include <Online/online.h>
#include <Internal/modloading.h>
#include <JSON/json.h>

namespace {
const ModInstance::LobbySetting* FindModeSetting(const std::string& key) {
    for (const auto* mod : ModLoading::Instance().GetMods()) {
        if (!mod->IsActive()) continue;
        for (const auto& option : mod->session.lobby_settings)
            if (option.key == key) return &option;
    }
    return nullptr;
}
bool ValidChoice(const ModInstance::LobbySetting* setting, int value) {
    return setting && value >= 0 && static_cast<size_t>(value) < setting->choices.size();
}
}

bool Online::IsCampaignPlayMode() const {
    return !LobbyCampaign().empty() && LobbyMode() != "com-wolfire-arena" &&
        LobbyMode() != "sandbox" && LobbyCampaign() != "com-wolfire-sandbox-levels" && !LobbyBuild();
}

std::string Online::ModeSettingsKey() const { return lobby_build_ ? "build" : lobby_mode_; }
bool Online::ModeSettingAvailable(const std::string& modes) const {
    if (lobby_build_ || lobby_mode_.empty()) return false;
    if (modes.empty()) return true;
    const char* type = lobby_mode_ == "com-wolfire-arena" ? "arena" : IsCampaignPlayMode() ? "campaign" : "sandbox";
    // Compare whole comma-separated tokens, also for custom campaign IDs.
    return ("," + modes + ",").find("," + std::string(type) + ",") != std::string::npos;
}
Json::Value Online::LobbyModeSettings() const {
    Json::Value modes(Json::objectValue);
    for (const auto& mode : mode_settings_)
        for (const auto& entry : mode.second)
            if (ValidChoice(FindModeSetting(entry.first), entry.second)) modes[mode.first][entry.first] = entry.second;
    return modes;
}
bool Online::ReceiveLobbyModeSettings(const Json::Value& modes, bool validate_only) {
    if (!modes.isObject() || modes.size() > 128) return false;
    decltype(mode_settings_) incoming;
    for (const auto& mode : modes.getMemberNames()) {
        const auto& prefs = modes[mode];
        if (mode.empty() || mode.size() > 256 || !prefs.isObject() || prefs.size() > 1024) return false;
        for (const auto& key : prefs.getMemberNames()) {
            const auto* option = FindModeSetting(key);
            const auto& value = prefs[key];
            if (!value.isInt() || !ValidChoice(option, value.asInt())) return false;
            incoming[mode][key] = value.asInt();
        }
    }
    if (!validate_only) mode_settings_.swap(incoming);
    return true;
}
int Online::ModeSetting(const std::string& key) const {
    const auto* option = FindModeSetting(key);
    if (!option) return 0;
    const auto mode = mode_settings_.find(ModeSettingsKey());
    if (mode == mode_settings_.end()) return option->default_value;
    const auto setting = mode->second.find(key);
    if (setting == mode->second.end() || !ValidChoice(option, setting->second)) return option->default_value;
    return setting->second;
}
bool Online::SetModeSetting(const std::string& key, int value, PlayerID actor) {
    actor = ResolveLobbyActor(actor);
    const auto* option = FindModeSetting(key);
    if (!IsActive() || LobbyMigrationBusy() || !IsLobbyHost(actor) || !ValidChoice(option, value) ||
        !ModeSettingAvailable(option->modes) || (!LobbyWaiting() && !option->live)) return false;
    if (IsClient()) {
        Json::Value request; request["action"] = "mode_setting"; request["actor"] = actor;
        request["key"] = key; request["value"] = value;
        SubmitLobbyRequest(Json::FastWriter().write(request)); return true;
    }
    if (option->placement == "difficulty") BeforeChallengeRateChange();
    auto& settings = mode_settings_[ModeSettingsKey()];
    settings[key] = value;
    BroadcastLobby(); return true;
}
