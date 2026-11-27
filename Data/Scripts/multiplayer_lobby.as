// One lobby for a host, remote clients, and every couch's local players.
// License: Apache-2.0.
#include "menu_common.as"
#include "multiplayer_menu_style.as"
#include "music_load.as"
#include "multiplayer_lobby_modes.as"
#include "lobby_difficulty.as"

MusicLoad ml("Data/Music/menu.xml");
IMGUI@ imGUI;
const vec4 kLobbyTitle = vec4(0.95f, 0.78f, 0.35f, 1.0f);
const vec4 kLobbyDim = vec4(0.68f, 0.69f, 0.71f, 1.0f);
const vec4 kLobbyWarn = vec4(1.0f, 0.65f, 0.4f, 1.0f);
array<LobbyPlayer> lobby_players;
uint lobby_revision = 0;
string lobby_displayed_mode;
bool show_mode_cards = true;
string map_filter;
string notice;
bool leave_denied = false;
int editing_name = -1;
string name_buffer;
bool name_keyboard_open = false;
int small_panel = 1;
string preview_path;
TextureAssetRef preview_texture;
bool focus_lobby_form = true;
int lobby_leader_input = -1;
string lobby_visible_hint;

bool HasFocus() { return false; }

bool CanGoBack() {
    if(lobby_difficulty_open) {
        lobby_difficulty_open = false;
        focus_lobby_form = true;
        return false;
    }
    if(HasPopup()) {
        RemovePopup();
        return false;
    }
    if(editing_name >= 0) {
        editing_name = -1;
        return false;
    }
    if(LobbyTryLeave()) return true;
    notice = "Use the lobby host's or local leader's controller to leave.";
    leave_denied = true;
    return false;
}

void Initialize() {
    ClearControllerItems();
    lobby_leader_input = LobbyGetLocalLeaderInputID();
    SetControllerPlayer(lobby_leader_input);
    @imGUI = CreateIMGUI();
    imGUI.setup();
    setBackGround();
    PlaySong("overgrowth_main");
    LobbyReadModes();
    lobby_players = LobbyPlayers();
    lobby_revision = LobbySnapshotRevision();
    lobby_displayed_mode = LobbyGetMode();
    show_mode_cards = LobbySelectedMode() < 0;
    focus_lobby_form = true;
}

void Dispose() {
    ClearControllerItems();
    imGUI.clear();
    lobby_modes.resize(0);
    preview_texture.Clear();
    preview_path = "";
}

void Update() {
    LobbyUpdate();
    if(lobby_leader_input != LobbyGetLocalLeaderInputID()) {
        lobby_leader_input = LobbyGetLocalLeaderInputID();
        SetControllerPlayer(lobby_leader_input);
        focus_lobby_form = true;
        editing_name = -1;
    }
    // Native Dear ImGui records whichever local controller actually acted.
    // Keep legacy popup/back handling on that same physical input too.
    int current_input = LobbyGetCurrentInputID();
    if(current_input >= 0 && controller_player != current_input) SetControllerPlayer(current_input);
    if(leave_denied && LobbyCanLeave()) {
        notice = "";
        leave_denied = false;
    }
    if(HasPopup()) {
        LobbySetJoinInputEnabled(false);
        UpdateController();
        imGUI.update();
        return;
    }
    if(!Online_IsActive() && !LobbyMigrationBusy()) {
        this_ui.SendCallback("multiplayer_menu.as");
        return;
    }
    UpdateKeyboardMouse();
    imGUI.update();
    if(GetInputPressed(controller_player, "menu_cancel")) {
        if(editing_name >= 0) editing_name = -1;
        else if(lobby_difficulty_open && !ImGui_IsAnyPopupOpen()) { lobby_difficulty_open = false; focus_lobby_form = true; }
        else if(!ImGui_IsAnyPopupOpen()) this_ui.SendCallback("back");
    }
    uint revision = LobbySnapshotRevision();
    if(revision != lobby_revision || lobby_players.length() == 0) {
        if(!LobbyMigrationBusy() && lobby_maps_from_host != Online_IsHosting()) LobbyReadModes();
        lobby_players = LobbyPlayers();
        lobby_revision = revision;
        string mode = LobbyGetMode();
        if(mode != lobby_displayed_mode || (!LobbyIsHost() && mode != "")) {
            show_mode_cards = mode == "";
            map_filter = "";
            lobby_displayed_mode = mode;
        }
    }
    LobbySetJoinInputEnabled(!lobby_difficulty_open && !LobbyMigrationBusy() && editing_name < 0 && !ImGui_IsAnyItemActive() && !ImGui_IsAnyPopupOpen());
}

void Resize() { imGUI.doScreenResize(); setBackGround(); }
void ScriptReloaded() { Dispose(); Initialize(); }
void Draw() {}
void Init(string str) {}

int LocalPlayers() {
    int count = 0;
    for(uint i = 0; i < lobby_players.length(); ++i) {
        if(lobby_players[i].local && !lobby_players[i].bot) {
            ++count;
        }
    }
    return count;
}

string LobbyShortName(const string &in name) {
    if(name.length() <= 16) return name;
    uint end = 13;
    while(end > 0 && (name[end] & 0xC0) == 0x80) --end;
    return name.substr(0, end) + "...";
}

string LobbyRoleSummary() {
    string actor = "Player " + (LobbyCurrentLocalPlayerIndex() + 1);
    string host = "unassigned";
    string leader = "unassigned";
    uint current = LobbyCurrentPlayerID();
    for(uint i = 0; i < lobby_players.length(); ++i) {
        LobbyPlayer player = lobby_players[i];
        if(player.id == current) actor = LobbyShortName(player.name);
        if(player.host) host = LobbyShortName(player.name);
        if(player.local && player.leader) leader = LobbyShortName(player.name);
    }
    return "Menu: " + actor + "  /  Host: " + host + "  /  Local leader: " + leader;
}

void LobbyForm(const string &in title, const vec2 &in position, const vec2 &in size, float scale, bool title_bar = true) {
    ImGui_SetNextWindowPos(position, ImGuiSetCond_Always);
    ImGui_SetNextWindowSize(size, ImGuiSetCond_Always);
    int flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoTitleBar;
    ImGui_Begin(title, flags);
    MPWindowScale(scale);
    MPPanelBackground();
    if(title_bar) { MPHeading(title); }
    ImGui_PushTextWrapPos(0.0f);
}

void LobbyEndForm() {
    ImGui_PopTextWrapPos();
    ImGui_End();
}

void LobbyHint(const string &in description) {
    // Disabled controls still explain their permissions. Use window-aware
    // item bounds because IsItemHovered deliberately excludes disabled items.
    vec2 a = ImGui_GetItemRectMin();
    vec2 b = ImGui_GetItemRectMax();
    bool target = mp_pointer_active ? (ImGui_IsWindowHovered() && ImGui_IsMouseHoveringRect(a, b, true))
                                    : ImGui_IsItemFocused();
    if(target) lobby_visible_hint = description;
}

void DrawLobbyHint() {
    if(lobby_visible_hint == "") return;
    ImGui_PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui_BeginTooltip();
    ImGui_PushTextWrapPos(min(480.0f, ImGui_GetDisplaySize().x - 48.0f));
    ImGui_TextWrapped(lobby_visible_hint);
    ImGui_PopTextWrapPos();
    ImGui_EndTooltip();
    ImGui_PopStyleVar();
}

string LobbyInitialForm() {
    return ImGui_GetDisplaySize().x >= 1100.0f || small_panel == 1 ? "Players" :
           small_panel == 0 ? "Mode and map" : small_panel == 2 ? "Mode settings" : "Hosting options";
}

bool LobbyModeDetailsVisible() {
    return !show_mode_cards && LobbySelectedMode() >= 0;
}

void DrawModeForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Mode and map", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Mode and map") {
        ImGui_SetWindowFocus();
        focus_lobby_form = false;
    }
    bool can_edit = LobbyIsHost() && !LobbyMigrationBusy();
    int selected = LobbySelectedMode();
    if(selected >= 0 && !show_mode_cards) {
        ImGui_TextColored(kLobbyTitle, lobby_modes[selected].title);
        ImGui_BeginDisabled(!can_edit);
        if(MPButton("Change mode", vec2(-1.0f, 32.0f * scale))) {
            show_mode_cards = true;
        }
        LobbyHint("Choose another game mode. Host only.");
        ImGui_EndDisabled();
    } else {
        ImGui_TextColored(kLobbyDim, can_edit ? "Choose a game mode" : "The host chooses the game mode");
        ImGui_BeginDisabled(!can_edit);
        float width = ImGui_GetContentRegionAvailWidth();
        ImGui_Columns(2, false);
        for(uint i = 0; i < lobby_modes.length(); ++i) {
            LobbyModeChoice@ mode = lobby_modes[i];
            if(!mode.texture_loaded) {
                mode.texture = LoadTexture(mode.thumbnail, kMPTextureFlags);
                mode.texture_loaded = true;
            }
            float card_width = max(70.0f, width * 0.5f - 20.0f);
            ImGui_PushID(int(i));
            if(int(i) == selected) {
                ImGui_PushStyleColor(ImGuiCol_Button, vec4(0.35f, 0.28f, 0.13f, 1.0f));
            }
            bool picked = ImGui_ImageButton(mode.texture, vec2(card_width, card_width * 0.56f));
            string mode_hint = mode.build ? "Create or edit a shared level." :
                "Choose " + mode.title + ", then pick a map. Host only.";
            LobbyHint(mode_hint);
            picked = MPButton(mode.title, vec2(card_width + 8.0f, 48.0f * scale)) || picked;
            LobbyHint(mode_hint);
            if(picked && LobbyChooseMode(int(i))) {
                show_mode_cards = false;
                map_filter = "";
                notice = "";
            }
            if(int(i) == selected) {
                ImGui_PopStyleColor();
            }
            ImGui_PopID();
            ImGui_NextColumn();
        }
        ImGui_Columns(1);
        ImGui_EndDisabled();
    }

    selected = LobbySelectedMode();
    if(selected >= 0 && !show_mode_cards) {
        LobbyModeChoice@ mode = lobby_modes[selected];
        ImGui_Spacing();
        ImGui_Separator();
        ImGui_TextColored(kLobbyTitle, "Map");
        ImGui_BeginDisabled(!can_edit);
        ImGui_PushItemWidth(-1.0f);
        ImGui_InputText("##map_search", map_filter, 128);
        LobbyHint("Filter maps by name.");
        ImGui_PopItemWidth();
        if(map_filter == "") {
            ImGui_TextColored(kLobbyDim, "Filter maps by name");
        }
        float list_height = min(230.0f * scale, max(100.0f, size.y * 0.35f));
        ImGui_BeginChild("maps", vec2(0.0f, list_height), true);
        MPWindowScale(1.0f);
        string query = ToLowerCase(map_filter);
        int selected_map = LobbySelectedMap(mode);
        for(uint i = 0; i < mode.maps.length(); ++i) {
            if(query != "" && ToLowerCase(mode.maps[i].title).findFirst(query) < 0) {
                continue;
            }
            if(MPComboOption(mode.maps[i].title + "##map" + i, int(i) == selected_map)) {
                if(LobbyChooseMap(selected, int(i))) {
                    notice = "";
                }
            }
            LobbyHint("Choose " + mode.maps[i].title + " for the next game. Loads when the host starts.");
        }
        ImGui_EndChild();
        MPWindowScale(scale);
        ImGui_EndDisabled();
        if(selected_map >= 0) {
            LobbyMapChoice@ map = mode.maps[selected_map];
            ImGui_Spacing();
            ImGui_TextWrapped(map.title);
            if(preview_path != map.thumbnail) {
                preview_path = map.thumbnail;
                preview_texture = LoadTexture(preview_path, kMPTextureFlags);
            }
            float image_width = ImGui_GetContentRegionAvailWidth();
            ImGui_Image(preview_texture, vec2(image_width, image_width * 0.56f));
            LobbyHint("Map preview: " + map.title + ".");
        } else if(LobbyGetMap() != "") {
            ImGui_TextWrapped("Selected map: " + LobbyLevelTitle(LobbyGetMap()));
        }
    } else if(selected < 0 && !LobbyIsHost()) {
        ImGui_TextWrapped("Waiting for the host to choose a mode and map.");
    }
    LobbyEndForm();
}

void DrawPlayersForm(const vec2 &in position, const vec2 &in size, float scale) {
    bool compact_players = size.y < 400.0f;
    float footer_height = (compact_players ? 88.0f : 156.0f) * scale + 32.0f;
    bool fixed_footer = size.y >= footer_height + 120.0f * scale;
    float roster_height = fixed_footer ? size.y - footer_height : size.y;
    LobbyForm("Players", position, vec2(size.x, roster_height), scale, !compact_players);
    ImGui_TextColored(kLobbyDim, compact_players ? "Controllers: press any button. Keyboard: Enter." :
        "Unassigned controllers: press any button. Keyboard: press Enter to join.");
    LobbyHint("Press any button on an unassigned controller to join. Keyboard users press Enter.");
    ImGui_Spacing();
    int locals = LocalPlayers();
    bool show_handicaps = LobbyGetMode() != "" && !LobbyGetBuildMode() && LobbyGetModeSetting("progression") != 0;
    if(fixed_footer) ImGui_BeginChild("player_list", vec2(0), true);
    MPWindowScale(1.0f);
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(10.0f, 5.0f));
    if(focus_lobby_form && LobbyInitialForm() == "Players") {
        ImGui_SetWindowFocus();
    }
    float width = ImGui_GetContentRegionAvailWidth();
    ImGui_Columns(3, false);
    float team_width = min(width * 0.26f, 120.0f * scale);
    float screen_width = min(width * 0.25f, 110.0f * scale);
    ImGui_SetColumnWidth(0, width - team_width - screen_width);
    ImGui_SetColumnWidth(1, team_width);
    ImGui_TextColored(kLobbyDim, "Slot / Player");
    ImGui_NextColumn();
    ImGui_TextColored(kLobbyDim, "Team");
    ImGui_NextColumn();
    ImGui_TextColored(kLobbyDim, "Screen");
    ImGui_NextColumn();
    ImGui_Separator();
    array<string> teams;
    int team_choices = int(lobby_players.length());
    for(uint i = 0; i < lobby_players.length(); ++i) team_choices = max(team_choices, lobby_players[i].team);
    for(int i = 0; i < team_choices; ++i) {
        teams.insertLast("" + (i + 1));
    }
    array<string> screens;
    for(int i = 0; i < locals; ++i) {
        screens.insertLast("" + (i + 1));
    }
    for(uint i = 0; i < lobby_players.length(); ++i) {
        LobbyPlayer player = lobby_players[i];
        ImGui_PushID(int(player.id));
        ImGui_PushTextWrapPos(0.0f);
        ImGui_TextColored(player.local ? kLobbyTitle : vec4(0.9f, 0.9f, 0.9f, 1.0f), player.slot_index + " / " + player.name);
        ImGui_PopTextWrapPos();
        LobbyHint(player.bot ? "AI player with a name and team. No controller or screen required." :
            player.local ? "Local player. This computer's leader manages their name and screen." :
                           "Remote player. Their local leader manages their name and screens.");
        string association = player.placeholder ? "Available player slot" : player.bot ? "Bot" : (player.local ? "Couch P" : "Remote P") + (player.local_index + 1);
        if(player.host || player.leader) {
            association += "\n" + (player.host && player.leader ? "Host / Leader" : player.host ? "Host" : "Local leader");
        }
        if(!player.bot && !player.placeholder && !player.connected) {
            association += "\nDisconnected";
        }
        ImGui_TextColored(kLobbyDim, association);
        LobbyHint("Host: match decisions. Local leader: this computer's screens, names, and players.");
        array<string> slot_types = {"Bot", "Player"};
        float selector_y = ImGui_GetCursorScreenPos().y;
        int slot_type = player.bot ? 0 : 1;
        ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy() || (!player.bot && !player.placeholder));
        ImGui_PushItemWidth(-1.0f);
        if(MPCombo("##slot_type", slot_type, slot_types)) LobbySetSlotPlayer(player.id, slot_type == 1);
        ImGui_PopItemWidth();
        ImGui_EndDisabled();
        LobbyHint("A player placeholder waits for a joining human. The slot keeps its unit and upgrades.");
        array<string> assign_names = {"Assign player..."};
        array<uint> assign_ids;
        for(uint who = 0; who < lobby_players.length(); ++who) {
            if(!lobby_players[who].bot && !lobby_players[who].placeholder && lobby_players[who].id != player.id) {
                assign_names.insertLast(lobby_players[who].name + " / slot " + lobby_players[who].slot_index);
                assign_ids.insertLast(lobby_players[who].id);
            }
        }
        int assignment = 0;
        ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy() || assign_ids.length() == 0);
        ImGui_PushItemWidth(-1.0f);
        if(MPCombo("##slot_player", assignment, assign_names) && assignment > 0) LobbySwapSlots(assign_ids[assignment - 1], player.id);
        ImGui_PopItemWidth();
        ImGui_EndDisabled();
        LobbyHint("Move this human into the slot's upgraded unit. Occupants swap; unit upgrades stay with their slots.");
        if(show_handicaps && !player.bot) {
            bool handicap = LobbyGetPlayerHandicap(player.id);
            ImGui_BeginDisabled(!LobbyCanSetPlayerHandicap(player.id));
            if(MPCheckbox("XP handicap", handicap)) LobbySetPlayerHandicap(player.id, handicap);
            ImGui_EndDisabled();
            LobbyHint("Defeat awards committed XP to " + LobbyShortName(player.name) + " (slot " + player.slot_index + "). Shared upgrades credit their team's wallet.");
        }
        if(player.local || player.bot || player.placeholder) {
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanRenamePlayer(player.id));
            float action_width = ImGui_GetContentRegionAvailWidth();
            bool can_remove = !LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id);
            if(MPButton("Rename", vec2(can_remove ? (action_width - 8.0f) * 0.5f : -1.0f, 30.0f * scale))) {
                editing_name = int(player.id);
                name_buffer = player.name;
                name_keyboard_open = LobbyCurrentLocalPlayerIndex() != GetLocalKeyboardPlayerIndex();
            }
            LobbyHint("Rename this player. Local leaders name humans; the host names bots.");
            if(focus_lobby_form && LobbyInitialForm() == "Players") {
                ImGui_SetItemDefaultFocus();
                ImGui_SetKeyboardFocusHere(-1);
                focus_lobby_form = false;
            }
            ImGui_EndDisabled();
            if(can_remove) {
                ImGui_SameLine(0.0f, 8.0f);
                if(MPButton(player.bot || player.placeholder ? "Remove slot" : "Remove", vec2(-1.0f, 30.0f * scale))) {
                    LobbyRemovePlayer(player.id);
                }
                LobbyHint(player.bot ? "Remove this bot. Host only." :
                    "Remove this player and free their controller. Local leader only.");
            }
        } else if(!LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id)) {
            string removal_label = player.leader ? "Kick client" : "Remove";
            if(MPButton(removal_label, vec2(-1.0f, 30.0f * scale))) {
                LobbyRemovePlayer(player.id);
            }
            LobbyHint(player.leader ? "Disconnect this computer and all its players. Host only." :
                                     "Remove this remote player. Host only.");
        }
        if(!player.bot && !LobbyMigrationBusy() && LobbyCanPromoteHost(player.id)) {
            if(MPButton("Make host", vec2(-1.0f, 30.0f * scale))) LobbyPromoteHost(player.id);
            LobbyHint("Transfer match control to this player. Choosing a remote player also transfers network hosting.");
        }
        if(!player.bot && !LobbyMigrationBusy() && LobbyCanPromoteLeader(player.id)) {
            if(MPButton("Make local leader", vec2(-1.0f, 30.0f * scale))) LobbyPromoteLeader(player.id);
            LobbyHint("Let this player manage this computer's screens, names, and local players.");
        }
        ImGui_NextColumn();
        // Align the Team and Screen selectors so Left/Right stays on this row.
        ImGui_Dummy(vec2(0.0f, max(0.0f, selector_y - ImGui_GetCursorScreenPos().y - 5.0f)));
        int team = min(int(teams.length()) - 1, max(0, player.team - 1));
        ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanSetPlayerTeam(player.id));
        ImGui_PushItemWidth(-1.0f);
        if(MPTeamCombo(team, int(teams.length()))) {
            LobbySetPlayerTeam(player.id, team + 1);
        }
        LobbyHint("Choose a team and its color. Host permissions determine who can edit.");
        ImGui_PopItemWidth();
        ImGui_EndDisabled();
        ImGui_NextColumn();
        ImGui_Dummy(vec2(0.0f, max(0.0f, selector_y - ImGui_GetCursorScreenPos().y - 5.0f)));
        if(player.bot || player.placeholder) {
            ImGui_TextColored(kLobbyDim, "None");
            LobbyHint("Bots do not need screens.");
        } else if(player.local) {
            int screen = min(locals - 1, max(0, player.screen - 1));
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
            ImGui_PushItemWidth(-1.0f);
            if(MPCombo("##screen", screen, screens)) {
                LobbySetPlayerScreen(player.id, screen + 1);
            }
            LobbyHint("Choose a screen; occupied positions swap players. Local leader only.");
            ImGui_PopItemWidth();
            ImGui_EndDisabled();
        } else {
            array<string> remote_screen = {"None"};
            int none = 0;
            ImGui_BeginDisabled(true);
            ImGui_PushItemWidth(-1.0f);
            MPCombo("##screen", none, remote_screen);
            LobbyHint("This player's local leader assigns screens on their computer.");
            ImGui_PopItemWidth();
            ImGui_EndDisabled();
        }
        ImGui_NextColumn();
        ImGui_Separator();
        ImGui_PopID();
    }
    ImGui_Columns(1);
    ImGui_PopStyleVar();
    if(fixed_footer) {
        ImGui_EndChild();
        LobbyEndForm();
        // Keep actions visible while the roster scrolls and independently
        // accessible to the gamepad/keyboard window switcher.
        LobbyForm("Session actions", position + vec2(0.0f, roster_height),
                  vec2(size.x, footer_height), scale, false);
    } else {
        // Short viewports use one scrollable panel for players and actions.
        MPWindowScale(scale);
        ImGui_Spacing();
    }
    DrawLobbySessionActions(scale, LobbyIsHost() && !LobbyMigrationBusy(), compact_players);
    LobbyEndForm();
}

bool LobbyModeIsArena() {
    return LobbyGetMode() == "com-wolfire-arena" || LobbyGetCampaign() == "com-wolfire-arena";
}

bool LobbyModeIsCampaign() {
    return !LobbyModeIsArena() && !LobbyGetBuildMode() && LobbyGetCampaign() != "" &&
           LobbyGetCampaign() != "com-wolfire-sandbox-levels" && LobbyGetMode() != "sandbox";
}

void DrawModLobbyOptions() {
    ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy());
    array<LobbyOption>@ options = LobbyModOptions();
    for(uint i = 0; i < options.length(); ++i) {
        const LobbyOption option = options[i];
        array<string>@ choices = option.Choices();
        int value = LobbyGetModeSetting(option.key);
        ImGui_PushID(option.key);
        if(choices.length() == 0) {
            ImGui_TextColored(kLobbyDim, option.label);
            ImGui_PushItemWidth(-1.0f);
            if(ImGui_InputInt("##seed", value, 0, 0)) LobbySetModeSetting(option.key, max(0, value));
            ImGui_PopItemWidth();
            LobbyHint(option.description);
            if(MPButton("Randomize seed", vec2(-1, 32 * mp_window_scale))) LobbySetModeSetting(option.key, LobbyNewSeed(value));
        } else if(choices.length() == 2 && choices[0] == "Off" && choices[1] == "On") {
            bool enabled = value != 0;
            if(MPCheckbox(option.label, enabled)) LobbySetModeSetting(option.key, enabled ? 1 : 0);
        } else {
            ImGui_TextColored(kLobbyDim, option.label);
            ImGui_PushItemWidth(-1.0f);
            if(MPCombo("##choice", value, choices)) LobbySetModeSetting(option.key, value);
            ImGui_PopItemWidth();
        }
        if(option.description != "") LobbyHint(option.description);
        ImGui_PopID();
    }
    ImGui_EndDisabled();
}

void DrawModeSettingsForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Mode settings", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Mode settings") { ImGui_SetWindowFocus(); focus_lobby_form = false; }
    if(LobbyGetBuildMode()) {
        ImGui_TextWrapped("Build mode uses the level editor. Difficulty rules apply to play modes.");
        LobbyEndForm();
        return;
    }
    ImGui_TextColored(kLobbyTitle, LobbyScalingDifficulty() ? "Scaling difficulty" : "Traditional difficulty");
    ImGui_TextColored(kLobbyDim, "Base: " + GetConfigValueString("difficulty_preset"));
    if(MPButton("Difficulty", vec2(-1.0f, 44.0f * scale))) {
        lobby_difficulty_open = true;
        lobby_difficulty_focus = true;
    }
    LobbyHint(ModSessionFeature("scaling") ? "Choose the starting preset and configure optional enemy scaling." : "Choose the starting difficulty preset.");
    DrawModLobbyOptions();
    if(LobbyModeIsArena()) {
        ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy());
        bool random = ConfigHasKey("oo_arena_random") && GetConfigValueBool("oo_arena_random");
        if(MPCheckbox("Arena randomizer", random)) { SetConfigValueBool("oo_arena_random", random); SaveConfig(); }
        LobbyHint("Randomize Arena fighters' species, appearance and equipment independently of difficulty progression.");
        ImGui_EndDisabled();
    }
    LobbyEndForm();
}

void DrawLobbySessionActions(float scale, bool can_edit, bool compact = false) {
    bool ready = can_edit && LobbyCanStart();
    float half = (ImGui_GetContentRegionAvailWidth() - 10.0f) * 0.5f;
    ImGui_BeginDisabled(!ready);
    if(MPButton("Start game", vec2(compact ? half : -1.0f, (compact ? 40.0f : 48.0f) * scale))) {
        if(!LobbyStart()) notice = "The game could not start. Check the selection and connected players.";
    }
    LobbyHint("Start the selected mode and map with this roster.");
    ImGui_EndDisabled();
    if(compact) ImGui_SameLine(0.0f, 10.0f);
    if(LobbyIsHost() && !LobbyMigrationBusy() && MPButton("Load game",vec2(compact ? half : -1.0f,36.0f*scale)))
        this_ui.SendCallback("session_load_menu.as");
    bool can_leave = LobbyCanLeave();
    ImGui_BeginDisabled(!can_leave);
    if(MPButton(Online_IsHosting() ? "Close lobby" : "Leave lobby", vec2(compact ? half : -1.0f, (compact ? 40.0f : 36.0f) * scale))) {
        if(LobbyTryLeave()) this_ui.SendCallback("back");
        else notice = "The lobby could not close. Use the host or local leader.";
    }
    LobbyHint(Online_IsHosting() ? "Close the session for everyone. Transfer host first to let others keep playing." :
                                   "Disconnect this computer and its local players.");
    ImGui_EndDisabled();
    if(!LobbyIsHost()) ImGui_TextWrapped("Waiting for the host to start.");
    else if(!LobbyCanStart()) ImGui_TextWrapped("Choose a mode and map before starting.");
    if(notice != "") ImGui_TextColored(kLobbyWarn, notice);
    if(LobbyGetMigrationStatus() != "") ImGui_TextColored(kLobbyTitle, LobbyGetMigrationStatus());
    if(!can_leave && !leave_denied) ImGui_TextColored(kLobbyDim, "Use the host's or local leader's controller to leave.");
}

void DrawHostingForm(const vec2 &in position, const vec2 &in size, float scale, bool show_mode_settings) {
    LobbyForm("Hosting options", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Hosting options") {
        ImGui_SetWindowFocus();
        focus_lobby_form = false;
    }
    bool can_edit = LobbyIsHost() && !LobbyMigrationBusy();
    ImGui_BeginDisabled(!can_edit);
    if(MPButton("Add slot", vec2(-1.0f, 40.0f * scale))) { if(!LobbyAddSlot()) notice = "Could not add a slot."; }
    LobbyHint("Add a unit slot. It starts as a bot and can be switched to a player placeholder.");
    ImGui_EndDisabled();
    ImGui_Separator();
    // The form itself scrolls. A nested child would be a separate controller
    // navigation scope, trapping focus between its options and Start/Close.
    ImGui_PushTextWrapPos(0.0f);
    ImGui_BeginDisabled(!can_edit);
    if(show_mode_settings) {
        int armor_rule = GetConfigValueInt("armor_localized");
        int choice = armor_rule == 2 ? 1 : armor_rule == 1 ? 2 : 0;
        array<string> armor_rules = {"Legacy", "Weighted", "Localized"};
        ImGui_TextColored(kLobbyDim, "Armor protection");
        ImGui_PushItemWidth(-1.0f);
        if(MPCombo("##armor_model", choice, armor_rules)) {
            SetConfigValueInt("armor_localized", choice == 1 ? 2 : choice == 2 ? 1 : 0);
            SaveConfig();
        }
        ImGui_PopItemWidth();
        LobbyHint("Legacy uses whole-body metal armor. Weighted adds protection by material and body region. Localized checks the point of impact.");
        ImGui_Spacing();
    }
    bool all_pause = LobbyGetAllowAllPause();
    if(MPCheckbox("Everyone may pause", all_pause)) {
        LobbySetAllowAllPause(all_pause);
    }
    LobbyHint("Allow every player to pause. Off: only the host can pause.");
    ImGui_TextColored(kLobbyDim, "Otherwise only the lobby host may pause.");
    ImGui_Spacing();
    bool host_controls_teams = LobbyGetHostControlsTeams();
    if(MPCheckbox("Host controls teams", host_controls_teams)) LobbySetHostControlsTeams(host_controls_teams);
    LobbyHint("On: only the host changes teams. Off: players can change their own team in modes that use lobby teams.");
    ImGui_TextColored(kLobbyDim, "When off, players may change their own team in modes using lobby teams.");
    ImGui_Spacing();
    if(show_mode_settings) {
        bool force_teams = LobbyGetForceTeams();
        if(MPCheckbox("Force lobby teams", force_teams)) LobbySetForceTeams(force_teams);
        LobbyHint("Use lobby teams in Campaign and Arena. Sandbox, Freeplay and Survival always use lobby teams.");
        string team_hint = "This mode uses lobby teams.";
        if(LobbyGetMode() == "survival")
            team_hint = "Each team faces its own enemy waves.";
        else if(LobbyModeIsCampaign())
            team_hint = force_teams ? "Campaign teams ally with NPC factions; extras are hostile." : "The campaign keeps players allied.";
        else if(LobbyModeIsArena())
            team_hint = force_teams ? "Lobby teams override the mode's teams." : "Arena chooses team/FFA lineups each match.";
        ImGui_TextColored(kLobbyDim, team_hint);
        ImGui_Spacing();
    }
    if(LobbyIsHost()) {
        if(LobbyIsLocalOnly()) {
            ImGui_TextWrapped("Local party. Online hosting is off.");
            if(MPButton("Open to online players", vec2(-1.0f, 36.0f * scale))) {
                if(!LobbyOpenToNetwork()) notice = "Could not enable online hosting.";
            }
            LobbyHint("Allow online players to join this party.");
        } else {
            bool is_public = !ConfigHasKey("oo_public_game") || GetConfigValueBool("oo_public_game");
            if(MPCheckbox("Public game", is_public)) {
                SetConfigValueBool("oo_public_game", is_public);
                SaveConfig();
            }
            LobbyHint("Show this lobby in the Steam browser. Off: friends only. IP connections still work.");
        }
        ImGui_Separator();
        ImGui_TextWrapped("Players use team colors. The host uses brown fur.");
        if(!LobbyIsLocalOnly() && Online_HasFriendInviteOverlay() && MPButton("Invite friends", vec2(-1.0f, 36.0f * scale))) {
            Online_ActivateInviteDialog();
        }
        if(!LobbyIsLocalOnly() && Online_HasFriendInviteOverlay()) LobbyHint("Open Steam friend invitations.");
    } else {
        ImGui_TextWrapped("The host chooses visibility and session permissions.");
        ImGui_Spacing();
        ImGui_TextWrapped("Players use team colors. The host uses brown fur.");
    }
    ImGui_EndDisabled();
    ImGui_PopTextWrapPos();
    LobbyEndForm();
}

void DrawNameForm(float scale) {
    if(editing_name < 0) {
        return;
    }
    vec2 display = ImGui_GetDisplaySize();
    float width = min(440.0f * scale, display.x - 48.0f);
    ImGui_SetNextWindowPos(vec2((display.x - width) * 0.5f, display.y * 0.30f), ImGuiSetCond_Always);
    ImGui_SetNextWindowSize(vec2(width, 0.0f), ImGuiSetCond_Always);
    bool bot_name = false;
    for(uint i = 0; i < lobby_players.length(); ++i) if(lobby_players[i].id == uint(editing_name)) bot_name = lobby_players[i].bot;
    string heading = bot_name ? "Edit bot name" : "Edit player's name";
    ImGui_Begin("Edit player name", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    MPWindowScale(scale);
    MPPanelBackground();
    MPHeading(heading, 44.0f);
    ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanRenamePlayer(uint(editing_name)));
    ImGui_PushItemWidth(-1.0f);
    bool appearing = ImGui_IsWindowAppearing();
    if(appearing && !name_keyboard_open) {
        ImGui_SetKeyboardFocusHere();
    }
    bool enter = ImGui_InputText("##player_name", name_buffer, 64, ImGuiInputTextFlags_EnterReturnsTrue);
    LobbyHint("Edit the player's display name. Enter saves.");
    ImGui_PopItemWidth();
    if(MPButton(name_keyboard_open ? "Hide keyboard" : "On-screen keyboard", vec2(-1.0f, 30.0f * scale))) name_keyboard_open = !name_keyboard_open;
    LobbyHint("Show a keyboard for controller typing.");
    if(name_keyboard_open) {
        const string keys = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
        float key_width = (ImGui_GetContentRegionAvailWidth() - 7.0f * 6.0f) / 8.0f;
        for(uint i = 0; i < keys.length(); ++i) {
            if(i % 8 != 0) ImGui_SameLine(0.0f, 6.0f);
            if(MPButton(keys.substr(i, 1), vec2(key_width, 27.0f * scale)) && name_buffer.length() < 64) name_buffer += keys.substr(i, 1);
            if(i == 0 && appearing) { ImGui_SetItemDefaultFocus(); ImGui_SetKeyboardFocusHere(-1); }
        }
        float half = (ImGui_GetContentRegionAvailWidth() - 6.0f) * 0.5f;
        if(MPButton("Space", vec2(half, 30.0f * scale)) && name_buffer.length() < 64) name_buffer += " ";
        LobbyHint("Add a space.");
        ImGui_SameLine(0.0f, 6.0f);
        if(MPButton("Erase", vec2(half, 30.0f * scale)) && name_buffer.length() > 0) name_buffer.resize(name_buffer.length() - 1);
        LobbyHint("Erase the last character.");
    }
    if(MPButton("Save", vec2(130.0f * scale, 34.0f * scale)) || enter) {
        if(LobbyCanRenamePlayer(uint(editing_name)) && LobbySetPlayerName(uint(editing_name), name_buffer)) {
            editing_name = -1;
            notice = "";
        } else {
            notice = "Use a valid name. The local leader names humans; the host names bots.";
        }
    }
    LobbyHint("Save this name.");
    ImGui_EndDisabled();
    ImGui_SameLine();
    if(MPButton("Cancel", vec2(130.0f * scale, 34.0f * scale))) {
        editing_name = -1;
    }
    LobbyHint("Discard this name edit.");
    if(notice != "") {
        ImGui_TextWrapped(notice);
    }
    ImGui_End();
}

void DrawGUI() {
    imGUI.render();
    if(HasPopup()) {
        return;
    }
    lobby_visible_hint = "";
    MPStyleBegin();
    vec2 display = ImGui_GetDisplaySize();
    float sw = display.x, sh = display.y;
    float scale = sh >= 1000.0f ? 1.15f : 1.0f;
    float margin = sw >= 1000.0f ? 24.0f : 16.0f;
    float gap = 16.0f;
    float width = sw - margin * 2.0f;
    float x = (sw - width) * 0.5f;
    float top = max(24.0f, sh * 0.06f);
    float header_h = 120.0f * scale;
    float body_y = top + header_h + gap;
    float body_h = sh - body_y - margin;
    if(small_panel == 2 && !LobbyModeDetailsVisible()) {
        small_panel = 0;
        focus_lobby_form = true;
    }
    if(lobby_difficulty_open && (!LobbyModeDetailsVisible() || LobbyGetBuildMode())) {
        lobby_difficulty_open = false;
        lobby_difficulty_focus = false;
        focus_lobby_form = true;
    }
    ImGui_PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(16.0f, 16.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(10.0f, 10.0f));
    ImGui_BeginDisabled(lobby_difficulty_open);
    LobbyForm("Lobby", vec2(x, top), vec2(width, header_h), scale, false);
    ImGui_Columns(2, false);
    ImGui_SetColumnWidth(0, width - min(340.0f * scale, width * 0.44f));
    ImGui_PushMenuFont(true);
    ImGui_TextColored(kLobbyTitle, Online_IsHosting() ? "Host lobby" : "Joined lobby");
    ImGui_PopFont();
    if(sw >= 1000.0f) {
        ImGui_SameLine();
        ImGui_TextColored(kLobbyDim, " / " + lobby_players.length() + " slots / " + LocalPlayers() + " on this couch");
        ImGui_TextColored(kLobbyDim, LobbyRoleSummary());
    } else ImGui_TextColored(kLobbyDim, "" + lobby_players.length() + " slots / " + LocalPlayers() + " local");
    ImGui_NextColumn();
    ImGui_TextColored(kLobbyDim, "Joining players");
    int admission = LobbyGetAutoAdmission() ? 0 : 1;
    array<string> admission_options = {"Auto add slots", "Designated slots"};
    ImGui_BeginDisabled(!LobbyIsHost() || LobbyMigrationBusy());
    ImGui_PushItemWidth(-1.0f);
    if(MPCombo("##slot_admission", admission, admission_options)) LobbySetAutoAdmission(admission == 0);
    ImGui_PopItemWidth();
    ImGui_EndDisabled();
    LobbyHint("Applies to online players and local controllers. Designated admission requires an available player placeholder for each person.");
    ImGui_Columns(1);
    LobbyEndForm();

    if(sw >= 1100.0f) {
        bool show_mode_settings = LobbyModeDetailsVisible();
        bool four_columns = show_mode_settings && width >= 1580.0f * scale;
        float left_w = width * (four_columns ? 0.22f : 0.28f);
        float right_w = width * (four_columns ? 0.44f : 0.32f);
        float players_w = width - left_w - right_w - gap * 2.0f;
        DrawModeForm(vec2(x, body_y), vec2(left_w, body_h), scale);
        DrawPlayersForm(vec2(x + left_w + gap, body_y), vec2(players_w, body_h), scale);
        float right_x = x + left_w + gap + players_w + gap;
        float hosting_y = body_y;
        if(four_columns) {
            float settings_w = (right_w - gap) * 0.53f;
            DrawModeSettingsForm(vec2(right_x, body_y), vec2(settings_w, body_h), scale);
            right_x += settings_w + gap;
            right_w -= settings_w + gap;
        } else if(show_mode_settings) {
            float settings_h = (body_h - gap) * 0.56f;
            DrawModeSettingsForm(vec2(right_x, body_y), vec2(right_w, settings_h), scale);
            hosting_y += settings_h + gap;
        }
        DrawHostingForm(vec2(right_x, hosting_y), vec2(right_w, body_y + body_h - hosting_y), scale, show_mode_settings);
    } else {
        // Keep every form comfortably wide on smaller windows; the tab row
        // switches complete forms instead of squeezing their controls together.
        bool two_columns = sw >= 900.0f;
        float tab_area = two_columns ? (width - gap) * 0.51f : width;
        if(two_columns) {
            if(small_panel == 1) small_panel = 0;
            DrawPlayersForm(vec2(x + tab_area + gap, body_y), vec2(width - tab_area - gap, body_h), scale);
        }
        float tab_h = 68.0f;
        float tab_button_h = 36.0f;
        LobbyForm("##lobby_sections", vec2(x, body_y), vec2(tab_area, tab_h), scale, false);
        bool show_mode_settings = LobbyModeDetailsVisible();
        int tab_count = (show_mode_settings ? 4 : 3) - (two_columns ? 1 : 0);
        float tab_w = (ImGui_GetContentRegionAvailWidth() - (tab_count - 1) * 10.0f) / tab_count;
        bool compact_tabs = tab_w < 220.0f * scale;
        if(MPButton(compact_tabs ? "Modes" : "Mode and map", vec2(tab_w, tab_button_h))) { small_panel = 0; focus_lobby_form = true; }
        LobbyHint("Choose the game mode and map. Host only.");
        if(!two_columns) {
            ImGui_SameLine();
            if(MPButton("Players", vec2(tab_w, tab_button_h))) { small_panel = 1; focus_lobby_form = true; }
            LobbyHint("Manage players, names, teams, and this computer's screens.");
        }
        if(show_mode_settings) {
            ImGui_SameLine();
            if(MPButton(compact_tabs ? "Rules" : "Mode settings", vec2(tab_w, tab_button_h))) { small_panel = 2; focus_lobby_form = true; }
            LobbyHint("Configure the starting difficulty and rules for this mode.");
        }
        ImGui_SameLine();
        if(MPButton(compact_tabs ? "Host" : "Hosting options", vec2(tab_w, tab_button_h))) { small_panel = 3; focus_lobby_form = true; }
        LobbyHint("Manage session permissions, invitations, and slots.");
        LobbyEndForm();
        vec2 panel_position(x, body_y + tab_h + gap);
        vec2 panel_size(tab_area, body_h - tab_h - gap);
        if(small_panel == 0) { DrawModeForm(panel_position, panel_size, scale); }
        else if(small_panel == 1) { DrawPlayersForm(panel_position, panel_size, scale); }
        else if(small_panel == 2) { DrawModeSettingsForm(panel_position, panel_size, scale); }
        else { DrawHostingForm(panel_position, panel_size, scale, show_mode_settings); }
    }
    DrawNameForm(scale);
    ImGui_EndDisabled();
    DrawLobbyDifficultyForm(scale);
    DrawLobbyHint();
    ImGui_PopStyleVar(2);
    MPStyleEnd();
}
