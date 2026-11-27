// Host controls for the Difficulty Scaling mod. Apache-2.0.
bool LobbyScalingDifficulty() { return ModSessionFeature("scaling") && GetConfigValueBool("difficulty_scaling"); }

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

// Compact versions of the colored difficulty cards. The normal ImGui button
// retains controller focus and the stock menu sound/hover treatment.
int DifficultyRateButtons(const string &in key, int current) {
    array<string> names = {"Off", "Gentle", "Standard", "Brisk", "Nightmare"};
    array<vec4> colors = {vec4(0.45f,0.45f,0.45f,1), HexColor("#49EA86"),
                          HexColor("#FFD53D"), HexColor("#C00000"), HexColor("#944AE8")};
    float width = (ImGui_GetContentRegionAvailWidth()-24.0f)/5.0f;
    int result = -1;
    ImGui_PushID(key);
    ImGui_PushMenuFont(true);
    for(int i=0;i<5;++i) {
        if(i != 0) ImGui_SameLine(0.0f,6.0f);
        float text_width = ImGui_CalcTextSize(names[i]).x;
        ImGui_SetWindowFontScale(mp_window_scale*kMPTextScale*min(1.0f,(width-12.0f)/max(1.0f,text_width)));
        ImGui_PushStyleColor(ImGuiCol_Button, current == i ? colors[i] : vec4(0.10f,0.10f,0.10f,1));
        ImGui_PushStyleColor(ImGuiCol_ButtonHovered, colors[i]);
        ImGui_PushStyleColor(ImGuiCol_Text, current == i && i < 3 ? vec4(0.07f,0.07f,0.07f,1) : vec4(1));
        if(ImGui_Button(names[i],vec2(width,44.0f*mp_window_scale))) {
            result = i; PlaySoundGroup("Data/Sounds/hit/hit_hard.xml");
        }
        ImGui_PopStyleColor(3);
    }
    ImGui_SetWindowFontScale(mp_window_scale*kMPTextScale);
    ImGui_PopFont();
    ImGui_PopID();
    return result;
}

void DrawLobbyAIRate() {
    ImGui_TextColored(kLobbyTitle,"AI scaling");
    int rate = GetConfigValueBool("campaign_ai_scaling") ? max(0,min(4,GetConfigValueInt("campaign_ai_scaling_rate"))) : 0;
    int selected = DifficultyRateButtons("campaign_ai_scaling_rate",rate);
    if(selected >= 0) {
        SetConfigValueInt("campaign_ai_scaling_rate",selected);
        SetConfigValueBool("campaign_ai_scaling",selected != 0);
        SaveConfig();
    }
    LobbyHint("Occasional offensive variations on selected enemies; defensive skill and friendly bots stay authored. Gentle: 0.5x; Standard: 1x; Brisk: 1.5x; Nightmare: 2x. New strengths apply at the next save, load or match.");
}

void DifficultyFactor(const string &in key, const string &in label, const string &in hint, bool available = true) {
    bool value = GetConfigValueBool(key);
    ImGui_BeginDisabled(!available);
    if(MPCheckbox(label, value)) { SetConfigValueBool(key, value); SaveConfig(); }
    LobbyHint(hint);
    ImGui_EndDisabled();
}

void DrawModeRate(const string &in key, const string &in label, const string &in hint, bool enabled = true) {
    ImGui_TextColored(kLobbyTitle, label);
    int rate = max(0, min(4, LobbyGetModeSetting(key)));
    ImGui_BeginDisabled(!enabled);
    int selected = DifficultyRateButtons(key,rate);
    if(selected >= 0 && !LobbySetModeSetting(key,selected)) notice = "That mode setting could not be changed.";
    ImGui_EndDisabled();
    LobbyHint(hint + "\n\nOff disables it. Gentle: 0.5x. Standard: 1x. Brisk: 1.5x. Nightmare: 2x.");
}

void DrawLobbyPerformanceScaling() {
    ImGui_TextColored(kLobbyTitle, "Performance scaling");
    int choice = max(0, min(3, LobbyGetModeSetting("challenge_performance")));
    array<string> choices = {"Off", "On win", "On loss", "Forced-50"};
    ImGui_PushItemWidth(-1.0f);
    if(MPCombo("##challenge_performance", choice, choices)) LobbySetModeSetting("challenge_performance", choice);
    LobbyHint("On win makes future fights harder. On loss makes the next attempt harder after a complete human-party wipe. Forced-50 adds more difficulty on wins and eases retries after losses; individual deaths never count as losses.");
    if(choice != 0) {
        int rate = max(0, min(4, LobbyGetModeSetting("challenge_performance_rate")));
        ImGui_TextColored(kLobbyDim, "Strength");
        int selected = DifficultyRateButtons("challenge_performance_rate", rate);
        if(selected >= 0) LobbySetModeSetting("challenge_performance_rate", selected);
        LobbyHint("Standard: On win +4%; On loss +5% per complete party wipe. Forced-50: wins +8%, each loss eases that encounter's retries by 2.5%, up to 25%. Gentle halves these changes; Brisk multiplies them by 1.5; Nightmare doubles them.");
    }
    ImGui_PopItemWidth();
}

void DrawLobbyScalingControls() {
    if(LobbyScalingDifficulty()) {
        bool survival = LobbyGetMode() == "survival";
        ImGui_Separator();
        ImGui_TextColored(kLobbyTitle, "Scaling factors");
        DifficultyFactor("campaign_mp_scaling", survival ? "Party stats" : "Party stats and reinforcements",
            survival ? "Strengthen newly spawned enemies according to the size of the team they face. Bots count less than humans." :
            "More main-team humans and friendly bots strengthen campaign enemies and add matching reinforcements. Bots count less; enemy numbers grow logarithmically.", survival || LobbyModeIsCampaign());
        DifficultyFactor("campaign_armor_scaling", "Physical armor",
            "Adds wearable armor pieces to hostile NPCs as difficulty grows. Coverage starts late and grows gradually; authored equipment is preserved.");
        DifficultyFactor("campaign_weapon_scaling", survival ? "Weapons for wave enemies" : "Weapons for extra enemies",
            survival ? "From battery 4, difficulty can arm eligible wave enemies with a dagger, sword or spear." :
            "Give unarmed reinforcements a chance to carry a dagger, sword or spear as difficulty increases. Wolves never receive weapons.", survival || LobbyModeIsCampaign());
        DrawLobbyAIRate();
        ImGui_Spacing();
        ImGui_Separator();
        ImGui_TextColored(kLobbyTitle, "Progression");
        DrawModeRate("challenge_time", "Active time", "Standard: +5% enemy durability every five active minutes. Loading, shared pauses and cutscenes do not count.");
        if(survival || LobbyModeIsArena() || LobbyModeIsCampaign()) DrawModeRate("challenge_round", survival ? "Battery completion" : LobbyModeIsArena() ? "Match completion" : "Level completion",
            "Standard adds 8% enemy durability per completed campaign level, Arena match or Survival battery. Each completion counts once.");
        DrawLobbyPerformanceScaling();
        ImGui_Spacing();
        ImGui_TextColored(kLobbyDim, "Scaling changes enemies, while the selected starting preset remains the base. Friendly bots retain base stats.");
    } else {
        ImGui_TextWrapped("The original difficulty presets apply without party, equipment, AI or progression scaling. Choose Scaling above to configure those rules.");
    }
}
