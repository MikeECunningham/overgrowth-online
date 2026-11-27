#pragma once

#include <JSON/json.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

class TiXmlElement;
class Online;
namespace LocalProfiles {
bool RegisterMod(int sid, const TiXmlElement* settings, const TiXmlElement* subprofiles);
unsigned Revision();
void ModsChanged();
bool HasPrimary();
void UsePrimary(uint32_t slot);
std::string SettingsView();
std::string View(const std::string& name);
bool Edit(const std::string& action, const std::string& name, const std::string& key, const std::string& value);
bool SetModSetting(const std::string& mod, const std::string& key, int value);
int GetModSetting(const std::string& mod, const std::string& key);
std::string GetData(const std::string& name, const std::string& mod, const std::string& sub);
bool SetData(const std::string& name, const std::string& mod, const std::string& sub, const std::string& json);
std::string LobbyView(uint32_t seat, uint32_t actor);
bool CanEdit(uint32_t seat, uint32_t actor);
bool Change(uint32_t seat, const std::string& action, const std::string& key, const std::string& value);
void ResetLobby();
void SyncRoster();
void SetGuestName(uint32_t slot, const std::string& name);
void Swap(uint32_t first, uint32_t second);
void SaveSlots(Json::Value& slots);
void LoadSlots(const Json::Value& slots);
Json::Value PublicState();
void Receive(const Json::Value& state);
void Rejected(uint32_t seat);
std::string WeaponSkin(uint32_t slot, const std::string& category);
std::string Outfit(uint32_t slot, const std::string& species);
std::string NewID();
enum class ClearKind { CampaignLevel, ArenaMatch, SurvivalBattery };
void Award(ClearKind kind, const std::string& run, unsigned completion, const std::vector<int>* winners);
std::set<std::string> UpgradePool(bool& enabled);
}  // namespace LocalProfiles
