// Difficulty configuration uses the lobby's native navigation and Settings artwork.
// License: Apache-2.0.
bool lobby_difficulty_open = false;
bool lobby_difficulty_focus = false;

#include "lobby_scaling_controls.as"

void DrawLobbyBaseDifficulty() {
    ImGui_TextColored(kLobbyTitle, "Starting difficulty");
    array<string> presets = GetConfigValueOptions("difficulty_preset");
    string current = GetConfigValueString("difficulty_preset");
    ImGui_PushItemWidth(-1.0f);
    if(MPBeginCombo("##base_difficulty", current)) {
        for(uint i = 0; i < presets.length(); ++i) {
            bool selected = current == presets[i];
            if(MPComboOption(presets[i], selected)) {
                SetConfigValueString("difficulty_preset", presets[i]);
                SetConfigValueInt("difficulty_preset_value", int(i) + 1);
                SaveConfig();
            }
            if(selected) ImGui_SetItemDefaultFocus();
        }
        MPEndCombo();
    }
    ImGui_PopItemWidth();
    LobbyHint("Casual, Hardcore and Expert retain their original combat behavior, speed and assistance settings.");
}

void DrawLobbyDifficultyForm(float scale) {
    if(!lobby_difficulty_open) return;
    vec2 display = ImGui_GetDisplaySize();
    float sw = display.x, sh = display.y;
    vec2 size(min(920.0f * scale, sw - 40.0f), sh - 64.0f);
    bool scaling = LobbyScalingDifficulty();
    ImGui_PushStyleColor(ImGuiCol_WindowBg, vec4(0.02f, 0.02f, 0.02f, 0.98f));
    LobbyForm(scaling ? "Scaling difficulty" : "Traditional difficulty", vec2((sw - size.x) * 0.5f, 32.0f), size, scale);
    if(lobby_difficulty_focus) { ImGui_SetWindowFocus(); lobby_difficulty_focus = false; }
    bool edit = LobbyIsHost() && !LobbyMigrationBusy();
    ImGui_BeginDisabled(!edit);
    DrawLobbyDifficultyMode();
    DrawLobbyBaseDifficulty();
    ImGui_Spacing();
    DrawLobbyScalingControls();
    ImGui_EndDisabled();
    if(!edit) ImGui_TextColored(kLobbyDim, "The lobby host controls difficulty.");
    ImGui_Spacing();
    if(MPButton("Back to lobby", vec2(-1.0f, 44.0f * scale))) {
        lobby_difficulty_open = false;
        focus_lobby_form = true;
    }
    LobbyEndForm();
    ImGui_PopStyleColor();
}
