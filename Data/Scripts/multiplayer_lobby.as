// One lobby for a host, remote clients, and every couch's local players.
// License: Apache-2.0.
#include "menu_common.as"
#include "multiplayer_menu_style.as"
#include "music_load.as"
#include "multiplayer_lobby_modes.as"

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
int editing_name = -1;
string name_buffer;
int small_panel = 1;
string preview_path;
TextureAssetRef preview_texture;
bool focus_lobby_form = true;
int lobby_leader_input = -1;

bool HasFocus() { return false; }

bool CanGoBack() {
    if(HasPopup()) {
        RemovePopup();
        return false;
    }
    if(!LobbyCanManageLocal()) {
        return false;
    }
    if(editing_name >= 0) {
        editing_name = -1;
        return false;
    }
    LobbyLeave();
    return true;
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
    if(editing_name >= 0 && LobbyCanManageLocal() && GetInputPressed(controller_player, "menu_cancel")) {
        editing_name = -1;
    }
    uint revision = LobbySnapshotRevision();
    if(revision != lobby_revision || lobby_players.length() == 0) {
        lobby_players = LobbyPlayers();
        lobby_revision = revision;
        string mode = LobbyGetMode();
        if(mode != lobby_displayed_mode || (!LobbyIsHost() && mode != "")) {
            show_mode_cards = mode == "";
            map_filter = "";
            lobby_displayed_mode = mode;
        }
    }
    LobbySetJoinInputEnabled(!LobbyMigrationBusy() && editing_name < 0 && !ImGui_IsAnyItemActive() && !ImGui_IsAnyPopupOpen());
}

void Resize() { imGUI.doScreenResize(); setBackGround(); }
void ScriptReloaded() { Dispose(); Initialize(); }
void Draw() {}
void Init(string str) {}

int LocalPlayers() {
    int count = 0;
    for(uint i = 0; i < lobby_players.length(); ++i) {
        if(lobby_players[i].local) {
            ++count;
        }
    }
    return count;
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

string LobbyInitialForm() {
    return GetScreenWidth() >= 1100 || small_panel == 1 ? "Players" :
           small_panel == 0 ? "Mode and map" : "Hosting options";
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
            picked = MPButton(LobbyCardTitle(mode.title), vec2(card_width + 8.0f, 48.0f * scale)) || picked;
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
        ImGui_PopItemWidth();
        if(map_filter == "") {
            ImGui_TextColored(kLobbyDim, "Filter maps by name");
        }
        float list_height = min(230.0f * scale, max(100.0f, size.y * 0.35f));
        ImGui_BeginChild("maps", vec2(0.0f, list_height), true);
        MPWindowScale(1.0f);
        string query = ToLowerCase(map_filter);
        string current = LobbyGetMap();
        for(uint i = 0; i < mode.maps.length(); ++i) {
            if(query != "" && ToLowerCase(mode.maps[i].title).findFirst(query) < 0) {
                continue;
            }
            if(ImGui_Selectable(mode.maps[i].title + "##map" + i, mode.maps[i].path == current,
                                0, vec2(0.0f, 28.0f * scale))) {
                if(LobbyChooseMap(selected, int(i))) {
                    notice = "";
                }
            }
        }
        ImGui_EndChild();
        MPWindowScale(scale);
        ImGui_EndDisabled();
        int selected_map = LobbySelectedMap(mode);
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
        }
    } else if(selected < 0 && !LobbyIsHost()) {
        ImGui_TextWrapped("Waiting for the host to choose a mode and map.");
    }
    LobbyEndForm();
}

void DrawPlayersForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Players", position, size, scale);
    ImGui_TextColored(kLobbyDim, "Unassigned controllers: press any button. Keyboard: press Enter to join.");
    ImGui_Spacing();
    int locals = LocalPlayers();
    float footer_height = 92.0f * scale;
    float list_height = max(100.0f, ImGui_GetContentRegionAvail().y - footer_height);
    ImGui_BeginChild("player_list", vec2(0.0f, list_height), true);
    MPWindowScale(1.0f);
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(10.0f, 5.0f));
    if(focus_lobby_form && LobbyInitialForm() == "Players") {
        ImGui_SetWindowFocus();
    }
    ImGui_Columns(3, false);
    float width = ImGui_GetContentRegionAvailWidth();
    ImGui_SetColumnWidth(0, max(130.0f, size.x * 0.45f));
    ImGui_SetColumnWidth(1, max(70.0f, size.x * 0.22f));
    ImGui_TextColored(kLobbyDim, "Player");
    ImGui_NextColumn();
    ImGui_TextColored(kLobbyDim, "Team");
    ImGui_NextColumn();
    ImGui_TextColored(kLobbyDim, "Screen");
    ImGui_NextColumn();
    ImGui_Separator();
    array<string> teams;
    for(uint i = 0; i < lobby_players.length(); ++i) {
        teams.insertLast("" + (i + 1));
    }
    array<string> screens;
    for(int i = 0; i < locals; ++i) {
        screens.insertLast("" + (i + 1));
    }
    for(uint i = 0; i < lobby_players.length(); ++i) {
        LobbyPlayer player = lobby_players[i];
        ImGui_PushID(int(player.id));
        ImGui_TextColored(player.local ? kLobbyTitle : vec4(0.9f, 0.9f, 0.9f, 1.0f), player.name);
        string association = (player.local ? "This couch P" : "Remote P") + (player.local_index + 1);
        if(player.host) {
            association += " / lobby host";
        }
        if(player.leader) {
            association += " / local leader";
        }
        if(!player.connected) {
            association += " / disconnected";
        }
        ImGui_TextColored(kLobbyDim, association);
        if(player.local) {
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
            float action_width = ImGui_GetContentRegionAvailWidth();
            bool can_remove = !LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id);
            if(MPButton("Rename", vec2(can_remove ? (action_width - 8.0f) * 0.5f : -1.0f, 30.0f * scale))) {
                editing_name = int(player.id);
                name_buffer = player.name;
            }
            if(focus_lobby_form && LobbyInitialForm() == "Players") {
                ImGui_SetItemDefaultFocus();
                ImGui_SetKeyboardFocusHere(-1);
                focus_lobby_form = false;
            }
            ImGui_EndDisabled();
            if(can_remove) {
                ImGui_SameLine(0.0f, 8.0f);
                if(MPButton("Remove", vec2(-1.0f, 30.0f * scale))) {
                    LobbyRemovePlayer(player.id);
                }
            }
        } else if(!LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id)) {
            string removal_label = player.leader ? "Kick client" : "Remove";
            if(MPButton(removal_label, vec2(-1.0f, 30.0f * scale))) {
                LobbyRemovePlayer(player.id);
            }
        }
        if(!LobbyMigrationBusy() && LobbyCanPromoteHost(player.id)) {
            if(MPButton("Make host", vec2(-1.0f, 30.0f * scale))) LobbyPromoteHost(player.id);
        }
        if(!LobbyMigrationBusy() && LobbyCanPromoteLeader(player.id)) {
            if(MPButton("Make local leader", vec2(-1.0f, 30.0f * scale))) LobbyPromoteLeader(player.id);
        }
        ImGui_NextColumn();
        // Align the interactive controls with Edit name, so right/left on a
        // controller stays on this player's row rather than jumping a row.
        ImGui_Dummy(vec2(0.0f, 2.0f * ImGui_GetTextLineHeightWithSpacing() - 5.0f));
        int team = min(int(teams.length()) - 1, max(0, player.team - 1));
        ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanSetPlayerTeam(player.id));
        ImGui_PushItemWidth(-1.0f);
        if(MPTeamCombo(team, int(teams.length()))) {
            LobbySetPlayerTeam(player.id, team + 1);
        }
        ImGui_PopItemWidth();
        ImGui_EndDisabled();
        ImGui_NextColumn();
        ImGui_Dummy(vec2(0.0f, 2.0f * ImGui_GetTextLineHeightWithSpacing() - 5.0f));
        if(player.local) {
            int screen = min(locals - 1, max(0, player.screen - 1));
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
            ImGui_PushItemWidth(-1.0f);
            if(ImGui_Combo("##screen", screen, screens)) {
                LobbySetPlayerScreen(player.id, screen + 1);
            }
            ImGui_PopItemWidth();
            ImGui_EndDisabled();
        } else {
            array<string> remote_screen = {"None"};
            int none = 0;
            ImGui_BeginDisabled(true);
            ImGui_PushItemWidth(-1.0f);
            ImGui_Combo("##screen", none, remote_screen);
            ImGui_PopItemWidth();
            ImGui_EndDisabled();
        }
        ImGui_NextColumn();
        ImGui_Separator();
        ImGui_PopID();
    }
    ImGui_Columns(1);
    ImGui_PopStyleVar();
    ImGui_EndChild();
    MPWindowScale(scale);
    ImGui_Spacing();
    ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
    if(MPButton("Add local player (debug)", vec2(-1.0f, 36.0f * scale))) {
        if(!LobbyAddLocalPlayer()) {
            notice = "Couldn't add a local player.";
        }
    }
    ImGui_EndDisabled();
    ImGui_TextColored(kLobbyDim, "The local leader assigns this couch's screens and names. Team permissions are set by the host.");
    LobbyEndForm();
}

void DrawHostingForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Hosting options", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Hosting options") {
        ImGui_SetWindowFocus();
        focus_lobby_form = false;
    }
    bool can_edit = LobbyIsHost() && !LobbyMigrationBusy();
    float options_height = max(130.0f, ImGui_GetContentRegionAvail().y - 176.0f * scale);
    ImGui_BeginChild("hosting_options", vec2(0.0f, options_height), false);
    MPWindowScale(1.0f);
    ImGui_PushTextWrapPos(0.0f);
    ImGui_BeginDisabled(!can_edit);
    bool all_pause = LobbyGetAllowAllPause();
    if(MPCheckbox("Everyone may pause", all_pause)) {
        LobbySetAllowAllPause(all_pause);
    }
    ImGui_TextColored(kLobbyDim, "Otherwise only the lobby host may pause.");
    ImGui_Spacing();
    bool host_controls_teams = LobbyGetHostControlsTeams();
    if(MPCheckbox("Host controls teams", host_controls_teams)) LobbySetHostControlsTeams(host_controls_teams);
    ImGui_TextColored(kLobbyDim, "When off, players may change their own team in modes using lobby teams.");
    ImGui_Spacing();
    bool force_teams = LobbyGetForceTeams();
    if(MPCheckbox("Force lobby teams", force_teams)) {
        LobbySetForceTeams(force_teams);
    }
    string team_hint = "Choose a mode to see its team rules.";
    if(force_teams) {
        team_hint = "Lobby teams override the mode's teams.";
    } else if(LobbyGetCampaign() == "com-wolfire-arena") {
        team_hint = "Arena chooses team/FFA lineups each match.";
    } else if(LobbyGetBuildMode() || LobbyGetMode() == "sandbox" ||
              LobbyGetCampaign() == "com-wolfire-sandbox-levels") {
        team_hint = "This mode uses lobby teams.";
    } else if(LobbyGetMode() != "") {
        team_hint = "The campaign keeps players allied.";
    }
    ImGui_TextColored(kLobbyDim, team_hint);
    ImGui_Spacing();
    if(LobbyIsHost()) {
        if(LobbyIsLocalOnly()) {
            ImGui_TextWrapped("Local party. Online hosting is off.");
            if(MPButton("Open to online players", vec2(-1.0f, 36.0f * scale))) {
                if(!LobbyOpenToNetwork()) notice = "Could not enable online hosting.";
            }
        } else {
            bool is_public = !ConfigHasKey("oo_public_game") || GetConfigValueBool("oo_public_game");
            if(MPCheckbox("Public game", is_public)) {
                SetConfigValueBool("oo_public_game", is_public);
                SaveConfig();
            }
        }
        if(LobbyGetCampaign() == "com-wolfire-arena") {
            int fighters = ConfigHasKey("oo_arena_bots") ? GetConfigValueInt("oo_arena_bots") : 2;
            array<string> bot_names = {"Off", "Up to 2", "Up to 3", "Up to 4", "Up to 6", "Up to 8"};
            array<int> bot_values = {0, 2, 3, 4, 6, 8};
            int bots = bot_values.find(fighters);
            if(bots < 0) { bots = 1; }
            ImGui_Text("Arena bots");
            ImGui_PushItemWidth(-1.0f);
            if(ImGui_Combo("##arena_bots", bots, bot_names)) {
                SetConfigValueInt("oo_arena_bots", bot_values[bots]);
                SaveConfig();
            }
            ImGui_PopItemWidth();
            bool random = ConfigHasKey("oo_arena_random") && GetConfigValueBool("oo_arena_random");
            if(MPCheckbox("Arena randomizer", random)) {
                SetConfigValueBool("oo_arena_random", random);
                SaveConfig();
            }
        }
        ImGui_Separator();
        ImGui_TextWrapped("Characters: Turner, with team colors.");
        ImGui_TextColored(kLobbyDim, "The lobby host keeps Turner's brown fur.");
        if(MPButton("Difficulty", vec2(-1.0f, 36.0f * scale))) {
            this_ui.SendCallback("difficulty_menu.as");
        }
        if(!LobbyIsLocalOnly() && Online_HasFriendInviteOverlay() && MPButton("Invite friends", vec2(-1.0f, 36.0f * scale))) {
            Online_ActivateInviteDialog();
        }
    } else {
        ImGui_TextWrapped("The host chooses visibility, difficulty and arena settings.");
        ImGui_Spacing();
        ImGui_TextWrapped("Characters: Turner, with team colors. The lobby host keeps Turner's brown fur.");
    }
    ImGui_EndDisabled();
    ImGui_PopTextWrapPos();
    ImGui_EndChild();
    MPWindowScale(scale);
    ImGui_Spacing();
    ImGui_Separator();
    bool ready = can_edit && !LobbyMigrationBusy() && LobbyCanStart();
    ImGui_BeginDisabled(!ready);
    if(MPButton("Start game", vec2(-1.0f, 48.0f * scale))) {
        if(!LobbyStart()) {
            notice = "The game could not start. Check the selection and connected players.";
        }
    }
    ImGui_EndDisabled();
    if(!LobbyIsHost()) {
        ImGui_TextWrapped("Waiting for the host to start.");
    } else if(!LobbyCanStart()) {
        ImGui_TextWrapped("Choose a mode and map before starting.");
    }
    if(notice != "") {
        ImGui_TextColored(kLobbyWarn, notice);
    }
    if(LobbyGetMigrationStatus() != "") ImGui_TextColored(kLobbyTitle, LobbyGetMigrationStatus());
    ImGui_Separator();
    ImGui_BeginDisabled(!LobbyCanManageLocal());
    if(MPButton(LobbyIsHost() ? "Close lobby" : "Leave lobby", vec2(-1.0f, 36.0f * scale))) {
        LobbyLeave();
        this_ui.SendCallback("back");
    }
    ImGui_EndDisabled();
    LobbyEndForm();
}

void DrawNameForm(float scale) {
    if(editing_name < 0) {
        return;
    }
    float width = min(440.0f * scale, float(GetScreenWidth()) - 48.0f);
    ImGui_SetNextWindowPos(vec2((float(GetScreenWidth()) - width) * 0.5f, float(GetScreenHeight()) * 0.30f), ImGuiSetCond_Always);
    ImGui_SetNextWindowSize(vec2(width, 0.0f), ImGuiSetCond_Always);
    ImGui_Begin("Edit local player's name", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    MPWindowScale(scale);
    MPPanelBackground();
    MPHeading("Edit local player's name", 44.0f);
    ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
    ImGui_PushItemWidth(-1.0f);
    if(ImGui_IsWindowAppearing()) {
        ImGui_SetKeyboardFocusHere();
    }
    bool enter = ImGui_InputText("##player_name", name_buffer, 64, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui_PopItemWidth();
    if(MPButton("Save", vec2(130.0f * scale, 34.0f * scale)) || enter) {
        if(LobbyCanManageLocal() && LobbySetPlayerName(uint(editing_name), name_buffer)) {
            editing_name = -1;
            notice = "";
        } else {
            notice = "Use a valid player name. Only this couch's P1 may rename its players.";
        }
    }
    ImGui_SameLine();
    if(MPButton("Cancel", vec2(130.0f * scale, 34.0f * scale))) {
        editing_name = -1;
    }
    if(notice != "") {
        ImGui_TextWrapped(notice);
    }
    ImGui_EndDisabled();
    ImGui_End();
}

void DrawGUI() {
    imGUI.render();
    if(HasPopup()) {
        return;
    }
    MPStyleBegin();
    float sw = float(GetScreenWidth());
    float sh = float(GetScreenHeight());
    float scale = sh >= 1000.0f ? 1.15f : 1.0f;
    float margin = sw >= 1000.0f ? 24.0f : 16.0f;
    float gap = 16.0f;
    float width = min(1720.0f, sw - margin * 2.0f);
    float x = (sw - width) * 0.5f;
    float top = max(24.0f, sh * 0.06f);
    float header_h = 62.0f * scale;
    float body_y = top + header_h + gap;
    float body_h = sh - body_y - margin;
    ImGui_PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(16.0f, 16.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(10.0f, 10.0f));
    LobbyForm("Lobby", vec2(x, top), vec2(width, header_h), scale, false);
    ImGui_PushMenuFont(true);
    ImGui_TextColored(kLobbyTitle, LobbyIsHost() ? "Host lobby" : "Joined lobby");
    ImGui_PopFont();
    ImGui_SameLine();
    ImGui_TextColored(kLobbyDim, " / " + lobby_players.length() + " players / " + LocalPlayers() + " on this couch");
    LobbyEndForm();

    if(sw >= 1100.0f) {
        float left_w = width * 0.31f;
        float right_w = width * 0.23f;
        float players_w = width - left_w - right_w - gap * 2.0f;
        DrawModeForm(vec2(x, body_y), vec2(left_w, body_h), scale);
        DrawPlayersForm(vec2(x + left_w + gap, body_y), vec2(players_w, body_h), scale);
        DrawHostingForm(vec2(x + left_w + gap + players_w + gap, body_y), vec2(right_w, body_h), scale);
    } else {
        // Keep every form comfortably wide on smaller windows; the tab row
        // switches complete forms instead of squeezing their controls together.
        float tab_h = 66.0f;
        LobbyForm("##lobby_sections", vec2(x, body_y), vec2(width, tab_h), scale, false);
        float tab_w = (ImGui_GetContentRegionAvailWidth() - 20.0f) / 3.0f;
        if(MPButton("Mode and map", vec2(tab_w, 28.0f))) { small_panel = 0; focus_lobby_form = true; }
        ImGui_SameLine();
        if(MPButton("Players", vec2(tab_w, 28.0f))) { small_panel = 1; focus_lobby_form = true; }
        ImGui_SameLine();
        if(MPButton("Hosting options", vec2(tab_w, 28.0f))) { small_panel = 2; focus_lobby_form = true; }
        LobbyEndForm();
        vec2 panel_position(x, body_y + tab_h + gap);
        vec2 panel_size(width, body_h - tab_h - gap);
        if(small_panel == 0) { DrawModeForm(panel_position, panel_size, scale); }
        else if(small_panel == 1) { DrawPlayersForm(panel_position, panel_size, scale); }
        else { DrawHostingForm(panel_position, panel_size, scale); }
    }
    DrawNameForm(scale);
    ImGui_PopStyleVar(2);
    MPStyleEnd();
}
