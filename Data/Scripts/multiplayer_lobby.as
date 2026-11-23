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
bool leave_denied = false;
int editing_name = -1;
string name_buffer;
bool name_keyboard_open = false;
int small_panel = 1;
string preview_path;
TextureAssetRef preview_texture;
bool focus_lobby_form = true;
int lobby_leader_input = -1;
bool lobby_hint_has_hover = false;

bool HasFocus() { return false; }

bool CanGoBack() {
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
        else if(!ImGui_IsAnyPopupOpen()) this_ui.SendCallback("back");
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
    bool hovered = ImGui_IsItemHovered() || (ImGui_IsWindowHovered() && ImGui_IsMouseHoveringRect(a, b, true));
    if(!hovered && !ImGui_IsItemFocused()) return;
    if(hovered) lobby_hint_has_hover = true;
    else if(lobby_hint_has_hover) return;
    ImGui_PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
    ImGui_BeginTooltip();
    ImGui_PushTextWrapPos(min(380.0f, float(GetScreenWidth()) - 48.0f));
    ImGui_TextWrapped(description);
    ImGui_PopTextWrapPos();
    ImGui_EndTooltip();
    ImGui_PopStyleVar();
}

string LobbyInitialForm() {
    return GetScreenWidth() >= 1100 || small_panel == 1 ? "Players" :
           small_panel == 0 ? "Mode and map" : small_panel == 2 ? "Mode settings" : "Hosting options";
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
        LobbyHint("Choose another game mode without starting or loading it. Only the lobby host can change the mode.");
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
            string mode_hint = mode.build ? "Open a shared level editor. Match difficulty rules apply only to play modes." :
                "Choose " + mode.title + ". Select a map and configure its Mode settings before starting. Only the lobby host can choose.";
            LobbyHint(mode_hint);
            picked = MPButton(LobbyCardTitle(mode.title), vec2(card_width + 8.0f, 48.0f * scale)) || picked;
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
        LobbyHint("Filter the map list by name without changing the selected map. The lobby host chooses the map.");
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
            LobbyHint("Choose " + mode.maps[i].title + " for the next game. This does not load the map until the host selects Start game.");
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
            LobbyHint("Preview of the selected map: " + map.title + ".");
        }
    } else if(selected < 0 && !LobbyIsHost()) {
        ImGui_TextWrapped("Waiting for the host to choose a mode and map.");
    }
    LobbyEndForm();
}

void DrawPlayersForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Players", position, size, scale);
    ImGui_TextColored(kLobbyDim, "Unassigned controllers: press any button. Keyboard: press Enter to join.");
    LobbyHint("An unassigned controller can join this computer's party by pressing a button. Press Enter to add the keyboard if it is not already assigned.");
    ImGui_Spacing();
    int locals = LocalPlayers();
    float footer_height = (LobbyIsHost() ? 136.0f : 92.0f) * scale;
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
        LobbyHint(player.bot ? "An AI roster player. The host chooses its name and team; it does not need a controller or screen." :
            player.local ? "A player on this computer. The local leader manages their name and screen; the host sets team permissions." :
                           "A player on another computer. Their local leader manages their name, controls and screens.");
        string association = player.bot ? "Bot" : (player.local ? "This couch P" : "Remote P") + (player.local_index + 1);
        if(player.host) {
            association += " / lobby host";
        }
        if(player.leader) {
            association += " / local leader";
        }
        if(!player.bot && !player.connected) {
            association += " / disconnected";
        }
        ImGui_TextColored(kLobbyDim, association);
        LobbyHint("The lobby host controls match decisions. Each computer's local leader manages that computer's names, screens and local player removal.");
        if(player.local || player.bot) {
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanRenamePlayer(player.id));
            float action_width = ImGui_GetContentRegionAvailWidth();
            bool can_remove = !LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id);
            if(MPButton("Rename", vec2(can_remove ? (action_width - 8.0f) * 0.5f : -1.0f, 30.0f * scale))) {
                editing_name = int(player.id);
                name_buffer = player.name;
                name_keyboard_open = LobbyCurrentLocalPlayerIndex() != GetLocalKeyboardPlayerIndex();
            }
            LobbyHint("Change the displayed name. The local leader names humans on this computer; the host names bots.");
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
                LobbyHint(player.bot ? "Remove this AI player from the roster. Only the lobby host can remove bots." :
                    "Remove this local player and free their controller. The local leader may remove local seats; promote another player before removing a protected host or leader.");
            }
        } else if(!LobbyMigrationBusy() && LobbyCanRemovePlayer(player.id)) {
            string removal_label = player.leader ? "Kick client" : "Remove";
            if(MPButton(removal_label, vec2(-1.0f, 30.0f * scale))) {
                LobbyRemovePlayer(player.id);
            }
            LobbyHint(player.leader ? "Disconnect this remote computer and all of its local players. Only the lobby host may kick a remote client." :
                                     "Remove this remote local seat from the roster. Only the lobby host may remove remote players.");
        }
        if(!player.bot && !LobbyMigrationBusy() && LobbyCanPromoteHost(player.id)) {
            if(MPButton("Make host", vec2(-1.0f, 30.0f * scale))) LobbyPromoteHost(player.id);
            LobbyHint("Give this player control over lobby and match decisions. A remote promotion also transfers network hosting so the original hosting computer may leave.");
        }
        if(!player.bot && !LobbyMigrationBusy() && LobbyCanPromoteLeader(player.id)) {
            if(MPButton("Make local leader", vec2(-1.0f, 30.0f * scale))) LobbyPromoteLeader(player.id);
            LobbyHint("Give this local player authority over this computer's screen positions, player names and local player removal. This does not transfer lobby-host authority.");
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
        LobbyHint("Choose this player's alliance and team color. The host may edit every team; humans may edit their own team when permitted and when the mode uses lobby teams. Bots remain host-controlled.");
        ImGui_PopItemWidth();
        ImGui_EndDisabled();
        ImGui_NextColumn();
        ImGui_Dummy(vec2(0.0f, 2.0f * ImGui_GetTextLineHeightWithSpacing() - 5.0f));
        if(player.bot) {
            ImGui_TextColored(kLobbyDim, "None");
            LobbyHint("Bots do not need screens or local input slots.");
        } else if(player.local) {
            int screen = min(locals - 1, max(0, player.screen - 1));
            ImGui_BeginDisabled(LobbyMigrationBusy() || !LobbyCanManageLocal());
            ImGui_PushItemWidth(-1.0f);
            if(ImGui_Combo("##screen", screen, screens)) {
                LobbySetPlayerScreen(player.id, screen + 1);
            }
            LobbyHint("Choose this local player's split-screen position. Occupied positions swap players. Only this computer's local leader can rearrange screens.");
            ImGui_PopItemWidth();
            ImGui_EndDisabled();
        } else {
            array<string> remote_screen = {"None"};
            int none = 0;
            ImGui_BeginDisabled(true);
            ImGui_PushItemWidth(-1.0f);
            ImGui_Combo("##screen", none, remote_screen);
            LobbyHint("This remote player uses a screen on their own computer. Their local leader assigns it there.");
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
    if(LobbyIsHost()) {
        ImGui_BeginDisabled(LobbyMigrationBusy());
        if(MPButton("Add bot", vec2(-1.0f, 36.0f * scale))) {
            if(!LobbyAddBot()) notice = "Couldn't add a bot.";
            else notice = "";
        }
        LobbyHint("Add an AI roster player with its own name and team, without a controller or screen. Only the lobby host can add bots.");
        ImGui_EndDisabled();
    }
    ImGui_TextColored(kLobbyDim, "The local leader assigns this couch's screens and names. Team permissions are set by the host.");
    LobbyEndForm();
}

bool LobbyModeIsArena() {
    return LobbyGetMode() == "com-wolfire-arena" || LobbyGetCampaign() == "com-wolfire-arena";
}

bool LobbyModeIsCampaign() {
    return !LobbyModeIsArena() && !LobbyGetBuildMode() && LobbyGetCampaign() != "" &&
           LobbyGetCampaign() != "com-wolfire-sandbox-levels" && LobbyGetMode() != "sandbox";
}

void DrawModeRate(const string &in key, const string &in label, const string &in hint, bool enabled = true) {
    ImGui_TextColored(kLobbyTitle, label);
    int rate = max(0, min(3, LobbyGetModeSetting(key)));
    array<string> rates = {"Off", "Gentle", "Standard", "Brisk"};
    ImGui_BeginDisabled(!enabled);
    ImGui_PushItemWidth(-1.0f);
    if(ImGui_Combo("##" + key, rate, rates) && !LobbySetModeSetting(key, rate)) {
        notice = "That mode setting could not be changed.";
    }
    ImGui_PopItemWidth();
    ImGui_EndDisabled();
    LobbyHint(hint + "\n\nOff disables this driver. Gentle uses half the Standard rate; Brisk uses 1.5 times the rate. Enemy NPCs and hostile roster bots scale; humans and friendly bots keep their stats. AI is unchanged.");
}

void DrawModeSettingsForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Mode settings", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Mode settings") {
        ImGui_SetWindowFocus();
        focus_lobby_form = false;
    }
    int selected = LobbySelectedMode();
    if(selected < 0 && LobbyGetMode() == "" && LobbyGetCampaign() == "") {
        ImGui_TextColored(kLobbyDim, "Choose a mode to see its settings.");
        LobbyEndForm();
        return;
    }
    string title = selected >= 0 ? lobby_modes[selected].title :
                   LobbyGetMode() == "sandbox" ? "Sandbox" : LobbyGetMode();
    ImGui_TextColored(kLobbyTitle, title);
    if(LobbyGetBuildMode()) {
        ImGui_TextWrapped("Build mode uses the level editor. Match difficulty rules apply to play modes.");
        LobbyEndForm();
        return;
    }
    bool can_edit = LobbyIsHost() && !LobbyMigrationBusy();
    ImGui_BeginDisabled(!can_edit);
    ImGui_TextColored(kLobbyDim, "Optional NPC challenge rules");
    DrawModeRate("challenge_time", "Time pressure", "Gradually strengthen opposing NPCs during active play. Standard adds 5% challenge health per five minutes. Pauses, loading, ready screens and cutscenes do not count.");
    if(LobbyModeIsArena() || LobbyModeIsCampaign()) {
        DrawModeRate("challenge_round", LobbyModeIsArena() ? "Completed matches" : "Completed levels",
                     LobbyModeIsArena() ? "Standard adds 8% challenge health after each resolved Arena match, regardless of winner." :
                                          "Standard adds 8% challenge health after each completed campaign level. Retrying an already credited level does not award it again in this run.");
    }
    DrawModeRate("challenge_win", "Encounter wins", "Standard adds 4% challenge health when the main character's team wins an encounter. Arena rounds, campaign defeat objectives and completed combat groups count.");
    bool adaptive = LobbyGetModeSetting("challenge_adaptive") != 0;
    DrawModeRate("challenge_death", "Player defeats", "Standard adds 5% challenge pressure when a human on the main character's team is knocked out or killed. A knockout followed by death counts once; bot defeats do not count. Adaptive wins replaces this separate increase.", !adaptive);
    bool wins_enabled = LobbyGetModeSetting("challenge_win") != 0;
    ImGui_BeginDisabled(!wins_enabled);
    if(MPCheckbox("Adaptive encounter wins", adaptive)) {
        if(!LobbySetModeSetting("challenge_adaptive", adaptive ? 1 : 0)) notice = "That mode setting could not be changed.";
    }
    LobbyHint("Reduce each encounter-win increase by 25% per human teammate defeat in that encounter, to zero after four. Failed retries retain these defeats. This discounts the future win award without reducing existing pressure. Requires Encounter wins and replaces Player defeats.");
    ImGui_EndDisabled();
    ImGui_TextColored(kLobbyDim, wins_enabled ? "Each human teammate defeat in an encounter reduces that win's increase by 25%." :
                                               "Enable encounter wins to use adaptive scaling.");
    if(adaptive) ImGui_TextColored(kLobbyDim, "Adaptive wins replaces the separate defeat pressure.");
    ImGui_Spacing();
    ImGui_TextColored(kLobbyDim, "Up to +150% challenge health, with total enemy durability capped at 6x. Damage and armor rise more gently; AI is unchanged.");
    ImGui_Separator();
    ImGui_TextColored(kLobbyTitle, "Base rules");
    if(LobbyModeIsArena()) {
        bool random = ConfigHasKey("oo_arena_random") && GetConfigValueBool("oo_arena_random");
        if(MPCheckbox("Arena randomizer", random)) {
            SetConfigValueBool("oo_arena_random", random);
            SaveConfig();
        }
        LobbyHint("Randomize Arena fighters' species, appearance and equipment when creating a match. This option is chosen by the lobby host.");
    }
    if(LobbyModeIsCampaign()) {
        bool party_scaling = ConfigHasKey("campaign_mp_scaling") && GetConfigValueBool("campaign_mp_scaling");
        if(MPCheckbox("Scale for party size", party_scaling)) {
            SetConfigValueBool("campaign_mp_scaling", party_scaling);
            SaveConfig();
        }
        LobbyHint("Make campaign enemies tougher and add capped same-species reinforcements as allied humans and lobby bots join. Allied bots retain the base difficulty. This is independent of progression rules.");
        ImGui_TextColored(kLobbyDim, "Counts allied humans and lobby bots.");
    }
    if(MPButton("Base difficulty", vec2(-1.0f, 36.0f * scale))) {
        this_ui.SendCallback("difficulty_menu.as");
    }
    LobbyHint("Choose the underlying combat difficulty. Optional party-size scaling and progression rules apply on top of it.");
    ImGui_EndDisabled();
    if(!LobbyIsHost()) ImGui_TextColored(kLobbyDim, "The lobby host chooses mode settings.");
    LobbyEndForm();
}

void DrawLobbySessionActions(float scale, bool can_edit) {
    bool ready = can_edit && LobbyCanStart();
    ImGui_BeginDisabled(!ready);
    if(MPButton("Start game", vec2(-1.0f, 48.0f * scale))) {
        if(!LobbyStart()) notice = "The game could not start. Check the selection and connected players.";
    }
    LobbyHint("Load the selected mode and map with this roster. The lobby host starts the game; starting creates a fresh progression challenge run.");
    ImGui_EndDisabled();
    if(!LobbyIsHost()) ImGui_TextWrapped("Waiting for the host to start.");
    else if(!LobbyCanStart()) ImGui_TextWrapped("Choose a mode and map before starting.");
    if(notice != "") ImGui_TextColored(kLobbyWarn, notice);
    if(LobbyGetMigrationStatus() != "") ImGui_TextColored(kLobbyTitle, LobbyGetMigrationStatus());
    bool can_leave = LobbyCanLeave();
    ImGui_BeginDisabled(!can_leave);
    if(MPButton(Online_IsHosting() ? "Close lobby" : "Leave lobby", vec2(-1.0f, 36.0f * scale))) {
        if(LobbyTryLeave()) this_ui.SendCallback("back");
        else notice = "The lobby could not close. Try again with the host or local leader.";
    }
    LobbyHint(Online_IsHosting() ? "Close this hosted session for everyone. Transfer hosting first if other computers should keep playing." :
                                   "Disconnect this computer and its local players from the lobby. Use the lobby host's or local leader's controller.");
    ImGui_EndDisabled();
    if(!can_leave && !leave_denied) ImGui_TextColored(kLobbyDim, "To leave, use the host's or local leader's controller.");
}

void DrawHostingForm(const vec2 &in position, const vec2 &in size, float scale) {
    LobbyForm("Hosting options", position, size, scale);
    if(focus_lobby_form && LobbyInitialForm() == "Hosting options") {
        ImGui_SetWindowFocus();
        focus_lobby_form = false;
    }
    bool can_edit = LobbyIsHost() && !LobbyMigrationBusy();
    DrawLobbySessionActions(scale, can_edit);
    ImGui_Separator();
    // The form itself scrolls. A nested child would be a separate controller
    // navigation scope, trapping focus between its options and Start/Close.
    ImGui_PushTextWrapPos(0.0f);
    ImGui_BeginDisabled(!can_edit);
    bool all_pause = LobbyGetAllowAllPause();
    if(MPCheckbox("Everyone may pause", all_pause)) {
        LobbySetAllowAllPause(all_pause);
    }
    LobbyHint("Allow any player to pause the shared world from their personal menu. Otherwise only the lobby host may pause. With multiple humans, opening personal menus keeps the world running.");
    ImGui_TextColored(kLobbyDim, "Otherwise only the lobby host may pause.");
    ImGui_Spacing();
    bool host_controls_teams = LobbyGetHostControlsTeams();
    if(MPCheckbox("Host controls teams", host_controls_teams)) LobbySetHostControlsTeams(host_controls_teams);
    LobbyHint("Only the lobby host may change teams. When off, each human may change their own team when the mode uses lobby teams. Bots remain host-controlled.");
    ImGui_TextColored(kLobbyDim, "When off, players may change their own team in modes using lobby teams.");
    ImGui_Spacing();
    bool force_teams = LobbyGetForceTeams();
    if(MPCheckbox("Force lobby teams", force_teams)) {
        LobbySetForceTeams(force_teams);
    }
    LobbyHint("Override the mode's team rules with these assignments. Off: campaign players and bots cooperate, Arena chooses team or free-for-all lineups, and sandbox still uses lobby teams. In forced campaigns, other teams join authored enemy factions in numeric order; surplus teams are hostile to every other team.");
    string team_hint = "Choose a mode to see its team rules.";
    if(force_teams) {
        if(LobbyGetCampaign() != "com-wolfire-arena" && !LobbyGetBuildMode() && LobbyGetMode() != "sandbox" &&
           LobbyGetCampaign() != "com-wolfire-sandbox-levels" && LobbyGetMode() != "")
            team_hint = "Campaign teams ally with NPC factions; extras are hostile.";
        else team_hint = "Lobby teams override the mode's teams.";
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
            LobbyHint("Enable remote connections for this local party, keeping its current players and settings.");
        } else {
            bool is_public = !ConfigHasKey("oo_public_game") || GetConfigValueBool("oo_public_game");
            if(MPCheckbox("Public game", is_public)) {
                SetConfigValueBool("oo_public_game", is_public);
                SaveConfig();
            }
            LobbyHint("Advertise this Steam lobby in the public browser. Off uses Steam friends visibility. Direct-IP joins remain available.");
        }
        ImGui_Separator();
        ImGui_TextWrapped("Characters: Turner, with team colors.");
        LobbyHint("Lobby players use Turner with their team color. The lobby host keeps Turner's brown fur.");
        ImGui_TextColored(kLobbyDim, "The lobby host keeps Turner's brown fur.");
        if(!LobbyIsLocalOnly() && Online_HasFriendInviteOverlay() && MPButton("Invite friends", vec2(-1.0f, 36.0f * scale))) {
            Online_ActivateInviteDialog();
        }
        if(!LobbyIsLocalOnly() && Online_HasFriendInviteOverlay()) LobbyHint("Open Steam's friend invitation overlay for this lobby. Steam support must be available.");
    } else {
        ImGui_TextWrapped("The host chooses visibility and session permissions.");
        ImGui_Spacing();
        ImGui_TextWrapped("Characters: Turner, with team colors. The lobby host keeps Turner's brown fur.");
        LobbyHint("Lobby players use Turner with their team color. The lobby host keeps Turner's brown fur.");
    }
    ImGui_EndDisabled();
    ImGui_PopTextWrapPos();
    LobbyEndForm();
}

void DrawNameForm(float scale) {
    if(editing_name < 0) {
        return;
    }
    float width = min(440.0f * scale, float(GetScreenWidth()) - 48.0f);
    ImGui_SetNextWindowPos(vec2((float(GetScreenWidth()) - width) * 0.5f, float(GetScreenHeight()) * 0.30f), ImGuiSetCond_Always);
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
    LobbyHint("Edit this player's displayed name. The draft is kept until you save or cancel; press Enter to save.");
    ImGui_PopItemWidth();
    if(MPButton(name_keyboard_open ? "Hide keyboard" : "On-screen keyboard", vec2(-1.0f, 30.0f * scale))) name_keyboard_open = !name_keyboard_open;
    LobbyHint("Show or hide the controller-friendly on-screen keyboard without changing the current name draft.");
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
        LobbyHint("Add a space to the name draft.");
        ImGui_SameLine(0.0f, 6.0f);
        if(MPButton("Erase", vec2(half, 30.0f * scale)) && name_buffer.length() > 0) name_buffer.resize(name_buffer.length() - 1);
        LobbyHint("Remove the last character from the name draft.");
    }
    if(MPButton("Save", vec2(130.0f * scale, 34.0f * scale)) || enter) {
        if(LobbyCanRenamePlayer(uint(editing_name)) && LobbySetPlayerName(uint(editing_name), name_buffer)) {
            editing_name = -1;
            notice = "";
        } else {
            notice = "Use a valid name. The local leader names humans; the host names bots.";
        }
    }
    LobbyHint("Save the displayed name. Local leaders name humans on this computer; the lobby host names bots.");
    ImGui_EndDisabled();
    ImGui_SameLine();
    if(MPButton("Cancel", vec2(130.0f * scale, 34.0f * scale))) {
        editing_name = -1;
    }
    LobbyHint("Discard this unsaved name draft and return to the lobby.");
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
    lobby_hint_has_hover = false;
    MPStyleBegin();
    float sw = float(GetScreenWidth());
    float sh = float(GetScreenHeight());
    float scale = sh >= 1000.0f ? 1.15f : 1.0f;
    float margin = sw >= 1000.0f ? 24.0f : 16.0f;
    float gap = 16.0f;
    float width = min(1720.0f, sw - margin * 2.0f);
    float x = (sw - width) * 0.5f;
    float top = max(24.0f, sh * 0.06f);
    float header_h = 94.0f * scale;
    float body_y = top + header_h + gap;
    float body_h = sh - body_y - margin;
    ImGui_PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(16.0f, 16.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(10.0f, 10.0f));
    LobbyForm("Lobby", vec2(x, top), vec2(width, header_h), scale, false);
    ImGui_PushMenuFont(true);
    ImGui_TextColored(kLobbyTitle, Online_IsHosting() ? "Host lobby" : "Joined lobby");
    ImGui_PopFont();
    ImGui_SameLine();
    ImGui_TextColored(kLobbyDim, " / " + lobby_players.length() + " players / " + LocalPlayers() + " on this couch");
    ImGui_TextColored(kLobbyDim, LobbyRoleSummary());
    LobbyEndForm();

    if(sw >= 1100.0f) {
        float left_w = width * 0.31f;
        float right_w = width * 0.23f;
        float players_w = width - left_w - right_w - gap * 2.0f;
        DrawModeForm(vec2(x, body_y), vec2(left_w, body_h), scale);
        DrawPlayersForm(vec2(x + left_w + gap, body_y), vec2(players_w, body_h), scale);
        float right_x = x + left_w + gap + players_w + gap;
        float settings_h = (body_h - gap) * 0.56f;
        DrawModeSettingsForm(vec2(right_x, body_y), vec2(right_w, settings_h), scale);
        DrawHostingForm(vec2(right_x, body_y + settings_h + gap), vec2(right_w, body_h - settings_h - gap), scale);
    } else {
        // Keep every form comfortably wide on smaller windows; the tab row
        // switches complete forms instead of squeezing their controls together.
        bool compact_tabs = sw < 800.0f;
        float tab_h = compact_tabs ? 82.0f : 66.0f;
        float tab_button_h = compact_tabs ? 44.0f : 28.0f;
        LobbyForm("##lobby_sections", vec2(x, body_y), vec2(width, tab_h), scale, false);
        float tab_w = (ImGui_GetContentRegionAvailWidth() - 30.0f) / 4.0f;
        if(MPButton(compact_tabs ? "Mode\nand map" : "Mode and map", vec2(tab_w, tab_button_h))) { small_panel = 0; focus_lobby_form = true; }
        LobbyHint("Choose the game mode and map. Only the lobby host may change this selection.");
        ImGui_SameLine();
        if(MPButton("Players", vec2(tab_w, tab_button_h))) { small_panel = 1; focus_lobby_form = true; }
        LobbyHint("View the party, rename players, assign teams and arrange this computer's screens.");
        ImGui_SameLine();
        if(MPButton(compact_tabs ? "Mode\nsettings" : "Mode settings", vec2(tab_w, tab_button_h))) { small_panel = 2; focus_lobby_form = true; }
        LobbyHint("View the selected mode's base rules and optional enemy challenge drivers. Only the lobby host may change them.");
        ImGui_SameLine();
        if(MPButton(compact_tabs ? "Hosting\noptions" : "Hosting options", vec2(tab_w, tab_button_h))) { small_panel = 3; focus_lobby_form = true; }
        LobbyHint("Manage session visibility, pause and team permissions, invitations, starting and leaving.");
        LobbyEndForm();
        vec2 panel_position(x, body_y + tab_h + gap);
        vec2 panel_size(width, body_h - tab_h - gap);
        if(small_panel == 0) { DrawModeForm(panel_position, panel_size, scale); }
        else if(small_panel == 1) { DrawPlayersForm(panel_position, panel_size, scale); }
        else if(small_panel == 2) { DrawModeSettingsForm(panel_position, panel_size, scale); }
        else { DrawHostingForm(panel_position, panel_size, scale); }
    }
    DrawNameForm(scale);
    ImGui_PopStyleVar(2);
    MPStyleEnd();
}
