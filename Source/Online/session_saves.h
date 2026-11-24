// Host-local save cards and restoration at gameplay boundaries. Apache-2.0.
#pragma once
#include <string>

namespace SessionSaves {
constexpr int kSlots = 7;  // autosave and six manual cards
bool Exists(int slot);
std::string Card(int slot, const std::string& field);
bool CanWrite();
bool Save(int slot);
bool Load(int slot);
std::string Status();
bool ResumingGameplay();
void SetCampaignTransition(bool pending, const std::string& destination);
bool CampaignTransition(std::string& destination);
void Autosave();
void SceneLoaded();
void SceneClosed();
void Update();
void CaptureFrame();
bool ResumeLobby(const std::string& mode, const std::string& campaign, const std::string& map, bool build);
void LeaveLobby();
int ArenaRound(int round);
int ArenaPoints(int avatar, int points);
void ArenaState(const std::string& state);
std::string ArenaRestore();
}  // namespace SessionSaves
