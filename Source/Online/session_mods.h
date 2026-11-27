// Host-owned mod state travels with a lobby or a host-local save. Apache-2.0.
#pragma once

#include <JSON/json.h>
#include <string>

namespace SessionMods {
enum class Scope { Network, Save, Handoff };  // Handoff includes private save-only data for the new host.
Json::Value Snapshot(Scope scope);
bool Validate(const Json::Value& mods, Scope scope, std::string& error);
void Restore(const Json::Value& mods, Scope scope);
void RestoreGameplay(const Json::Value& save);
bool ReceiveUpdate(const Json::Value& mods);
void PublishFeature(const std::string& feature);
void Clear();
std::string GetData(const std::string& id);
bool SetData(const std::string& id, const std::string& json);
void MigrateSave(Json::Value& save);
void KeepLobbyEdits(Json::Value& saved_mods);
void CloseLegacyRoundShop(Json::Value& saved_mods);
}
