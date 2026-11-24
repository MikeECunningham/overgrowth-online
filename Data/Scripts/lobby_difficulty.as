// Difficulty configuration uses the lobby's native navigation and Settings artwork.
// License: Apache-2.0.
bool lobby_difficulty_open = false;
bool lobby_difficulty_focus = false;

bool LobbyScalingDifficulty() { return GetConfigValueBool("difficulty_scaling"); }

void DrawLobbyDifficultyMode() {
    ImGui_TextColored(kLobbyDim, "Difficulty mode");
    int mode = LobbyScalingDifficulty() ? 1 : 0;
    array<string> modes = {"Traditional", "Scaling"};
    ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy());
    ImGui_PushItemWidth(-1.0f);
    if(MPCombo("##difficulty_mode", mode, modes)) {
        SetConfigValueBool("difficulty_scaling", mode == 1);
        SaveConfig();
    }
    ImGui_PopItemWidth();
    ImGui_EndDisabled();
    LobbyHint("Traditional uses the original difficulty presets. Scaling enables the party, equipment and progression rules configured under Difficulty.");
}

void DifficultyFactor(const string &in key, const string &in label, const string &in hint, bool available = true) {
    bool value = GetConfigValueBool(key);
    ImGui_BeginDisabled(!available);
    if(MPCheckbox(label, value)) { SetConfigValueBool(key, value); SaveConfig(); }
    LobbyHint(hint);
    ImGui_EndDisabled();
}

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
    LobbyHint("Casual, Hardcore and Expert retain their original combat behavior, speed and assistance settings. Optional scaling is applied separately.");
}

void DrawLobbyPerformanceScaling() {
    ImGui_TextColored(kLobbyTitle, "Performance scaling");
    int choice = max(0, min(3, LobbyGetModeSetting("challenge_performance")));
    array<string> choices = {"Off", "On win", "On loss", "Forced-50"};
    ImGui_PushItemWidth(-1.0f);
    if(MPCombo("##challenge_performance", choice, choices)) LobbySetModeSetting("challenge_performance", choice);
    LobbyHint("On win makes future fights harder. On loss makes the next attempt harder after a complete human-party wipe. Forced-50 adds more difficulty on wins and eases retries after losses; individual deaths never count as losses.");
    if(choice != 0) {
        int rate = max(0, min(2, LobbyGetModeSetting("challenge_performance_rate") - 1));
        array<string> rates = {"Gentle", "Standard", "Brisk"};
        ImGui_TextColored(kLobbyDim, "Strength");
        if(MPCombo("##challenge_performance_rate", rate, rates)) LobbySetModeSetting("challenge_performance_rate", rate + 1);
        LobbyHint("Standard: On win +4%; On loss +5% per complete party wipe. Forced-50: wins +8%, each loss eases that encounter's retries by 2.5%, up to 25%. Gentle halves these changes; Brisk multiplies them by 1.5.");
    }
    ImGui_PopItemWidth();
}

void DrawLobbyDifficultyForm(float scale) {
    if(!lobby_difficulty_open) return;
    float sw = float(GetScreenWidth()), sh = float(GetScreenHeight());
    vec2 size(min(920.0f * scale, sw - 40.0f), sh - 64.0f);
    bool scaling = LobbyScalingDifficulty();
    ImGui_PushStyleColor(ImGuiCol_WindowBg, vec4(0.02f, 0.02f, 0.02f, 0.98f));
    LobbyForm(scaling ? "Scaling difficulty" : "Traditional difficulty", vec2((sw - size.x) * 0.5f, 32.0f), size, scale);
    if(lobby_difficulty_focus) { ImGui_SetWindowFocus(); lobby_difficulty_focus = false; }
    bool edit = LobbyIsHost() && !LobbyMigrationBusy();
    ImGui_BeginDisabled(!edit);
    DrawLobbyBaseDifficulty();
    ImGui_Spacing();
    if(scaling) {
        ImGui_Separator();
        ImGui_TextColored(kLobbyTitle, "Scaling factors");
        DifficultyFactor("campaign_mp_scaling", "Party stats and reinforcements",
            "More main-team humans and friendly bots strengthen campaign enemies and add matching reinforcements. Bots count less; enemy numbers grow logarithmically.", LobbyModeIsCampaign());
        DifficultyFactor("campaign_armor_scaling", "Physical armor",
            "Adds wearable armor pieces to hostile NPCs as difficulty grows. Coverage starts late and grows gradually; authored equipment is preserved.");
        DifficultyFactor("campaign_weapon_scaling", "Weapons for extra enemies",
            "Generated reinforcements start unarmed. Daggers appear first, then more armed enemies, and swords or spears late. Wolves never receive weapons.", LobbyModeIsCampaign());
        DifficultyFactor("campaign_ai_scaling", "AI scaling",
            "Occasionally gives an enemy a small offensive AI variation as difficulty grows. Preserves authored defensive skill and does not strengthen friendly bots.");
        ImGui_Spacing();
        ImGui_Separator();
        ImGui_TextColored(kLobbyTitle, "Progression");
        DrawModeRate("challenge_time", "Active time", "Standard: +5% enemy durability every five active minutes. Loading, shared pauses and cutscenes do not count.");
        if(LobbyModeIsArena() || LobbyModeIsCampaign()) DrawModeRate("challenge_round", LobbyModeIsArena() ? "Match completion" : "Level completion",
            "Standard: +8% enemy durability per completed match or level. Retrying does not count as another completion.");
        DrawLobbyPerformanceScaling();
        ImGui_Spacing();
        ImGui_TextColored(kLobbyDim, "Scaling changes enemies, while the selected starting preset remains the base. Friendly bots retain base stats.");
    } else {
        ImGui_TextWrapped("The original difficulty presets apply without party, equipment, AI or progression scaling. Switch the lobby's Difficulty mode to Scaling to configure those rules.");
    }
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
