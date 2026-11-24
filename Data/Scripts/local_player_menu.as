// Each local player owns a separate instance of this script and settings tree.
// The native menu manager supplies that player's input and camera viewport.
#include "menu_common.as"
#include "settings.as"
#include "session_save_cards.as"

IMGUI@ imGUI;
bool capture_input = false;
bool personal_settings = false;
bool personal_teams = false;
bool personal_bot_name = false;
int editing_bot = -1;
TextBox bot_name_editor;
string bot_name_notice;
bool world_paused = false;
string local_player_notice;
uint player_menu_revision = 0;
int team_scroll_offset = 0;
const int kVisibleTeamRows = 4;

bool HasFocus() { return capture_input; }

void MenuBack() {
    if(save_cards_open) { BuildPlayerPauseMenu(); return; }
    if(HasPopup()) {
        RemovePopup();
    } else if(personal_bot_name) {
        if(bot_name_editor.active) {
            bot_name_editor.Deactivate();
            capture_input = false;
        } else BuildPlayerTeamsMenu();
    } else if(personal_settings) {
        ProcessSettingsMessage(IMMessage("back"));
    } else if(personal_teams) {
        if(OptionMenuOpen()) CloseAllOptionMenus();
        else BuildPlayerPauseMenu();
    } else {
        this_ui.SendCallback("close");
    }
}

bool CanGoBack() {
    MenuBack();
    return false; // Native callbacks close this player, never the engine stack.
}

void Initialize() {
    SetControllerPlayer(GetPlayerMenuInputID());
    @imGUI = CreateIMGUI();
    imGUI.setup();
    world_paused = GetPlayerMenuWorldPaused();
    BuildPlayerPauseMenu();
}

void Dispose() {
    ClearControllerItems();
    imGUI.clear();
    SaveConfig();
}

void BuildPlayerPauseMenu() {
    save_cards_open=false;
    ClearControllerItems();
    imGUI.clear();
    imGUI.setup();
    gui_elements.resize(0);
    category_elements.resize(0);
    personal_settings = false;
    personal_teams = false;
    personal_bot_name = false;
    capture_input = false;
    world_paused = GetPlayerMenuWorldPaused();
    player_menu_revision = LobbySnapshotRevision();
    SetControllerPlayer(GetPlayerMenuInputID());

    IMContainer panel(900.0f, 1300.0f);
    panel.setAlignment(CACenter, CACenter);
    IMImage background("Textures/ui/menus/main/inGameMenu-bg.png");
    background.setSize(vec2(700.0f, 1200.0f));
    background.setColor(vec4(0, 0, 0, 0.86f));
    panel.addFloatingElement(background, "player_menu_background", vec2(100.0f, 50.0f), 0);

    IMDivider buttons("player_menu_buttons", DOVertical);
    buttons.setSizeX(800.0f);
    buttons.setAlignment(CACenter, CACenter);
    panel.setElement(buttons);

    IMImage heading(brushstroke_background);
    heading.setSize(vec2(700.0f, 128.0f));
    IMContainer title(700.0f, 128.0f);
    title.setAlignment(CACenter, CACenter);
    title.addFloatingElement(heading, "heading_background", vec2(0), 1);
    IMText title_text("Player " + (GetPlayerMenuLocalIndex() + 1), button_font);
    title_text.setZOrdering(3);
    title.setElement(title_text);
    buttons.append(title);
    buttons.appendSpacer(25.0f);

    const bool shared_session = LobbyHasMultipleHumans() || GetLocalPlayerCount() > 1;
    if(shared_session && world_paused) {
        buttons.append(IMText("World paused", button_font_small));
        buttons.appendSpacer(15.0f);
    }

    const bool primary = LobbyCanManageLocal();
    const bool host = LobbyIsHost();
    AddButton("Continue", buttons, "null", button_background_diamond, false, 650.0f,
              30.0f, mouseover_scale_button, IMMessage("Continue"));
    AddButton("Settings", buttons, "null", button_background_diamond, false, 650.0f,
              30.0f, mouseover_scale_button, IMMessage("Settings"));
    if(Online_IsActive()) {
        AddButton("Teams", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Teams"));
    }
    if(shared_session && LobbyCanPause()) {
        AddButton(world_paused ? "Resume world" : "Pause world", buttons, "null", button_background_diamond,
                  false, 650.0f, 30.0f, mouseover_scale_button, IMMessage("toggle_world_pause"));
    }
    if(SessionCanManageSaves()) {
        if(SessionCanSave()) AddButton("Save game", buttons,"null",button_background_diamond,false,650,30,mouseover_scale_button,IMMessage("open_save_cards"));
        AddButton("Load game", buttons,"null",button_background_diamond,false,650,30,mouseover_scale_button,IMMessage("open_load_cards"));
    }
    if(host && !Online_IsClient()) {
        AddButton("Retry", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Retry"));
    }
    if(host && Online_IsHosting()) {
        AddButton("End game", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("end_game_to_lobby"));
    }
    if(primary && Online_IsClient()) {
        AddButton("Disconnect", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Main Menu"));
    }
    if(host && LobbyIsLocalOnly()) {
        AddButton("Open to online players", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("open_party_online"));
    }
    if(host && Online_IsHosting() && !LobbyIsLocalOnly() && Online_HasFriendInviteOverlay()) {
        AddButton("Invite friends", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Invite friends"));
    }
    if(host) {
        AddButton("Add slot", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("add_lobby_bot"));
    }
    if(primary && EditorEnabled()) {
        AddButton("Media mode", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Media Mode"));
    }
    if(primary) {
        AddButton("Main Menu", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Main Menu"));
    }
    if(local_player_notice != "") {
        IMText notice(local_player_notice, button_font_extra_small);
        buttons.append(notice);
    }
    buttons.appendSpacer(60.0f);
    imGUI.getMain().setElement(panel);
    controller_wraparound = true;
}

void OpenPersonalSettings() {
    ClearControllerItems();
    personal_teams = false;
    personal_bot_name = false;
    personal_settings = true;
    capture_input = false;
    SetControllerPlayer(GetPlayerMenuInputID());
    BuildUI(MenuOwnsKeyboard() ? "Controls" : "Gamepad");
}

class PlayerTeamConfiguration : DropdownConfigurationBaseClass {
    uint player_id;
    PlayerTeamConfiguration(uint id, int count) {
        player_id = id;
        @display_values = array<string>();
        for(int i = 1; i <= count; ++i) display_values.insertLast("" + i);
    }
    void SetCurrentValue(IMText@ label) {
        array<LobbyPlayer> players = LobbyPlayers();
        for(uint i = 0; i < players.length(); ++i) {
            if(players[i].id == player_id) {
                current_option_index = max(0, min(int(display_values.length()) - 1, players[i].team - 1));
                label.setText("Team " + players[i].team);
                return;
            }
        }
        label.setText("Disconnected");
    }
    void SetConfigValue(string value) {
        LobbySetPlayerTeam(player_id, parseInt(value));
    }
}

class PlayerScreenConfiguration : DropdownConfigurationBaseClass {
    uint player_id;
    PlayerScreenConfiguration(uint id, int count) {
        player_id = id;
        @display_values = array<string>();
        for(int i = 1; i <= count; ++i) display_values.insertLast("" + i);
    }
    void SetCurrentValue(IMText@ label) {
        array<LobbyPlayer> players = LobbyPlayers();
        for(uint i = 0; i < players.length(); ++i) {
            if(players[i].id == player_id) {
                current_option_index = max(0, players[i].screen - 1);
                label.setText("Screen " + players[i].screen);
                return;
            }
        }
        label.setText("None");
    }
    void SetConfigValue(string value) { LobbySetPlayerScreen(player_id, parseInt(value)); }
}

void PlayerTeamsLabel(const string &in text, IMDivider@ parent, bool heading = false) {
    IMContainer label(900.0f, heading ? 100.0f : 45.0f);
    label.setAlignment(heading ? CACenter : CALeft, CACenter);
    IMText caption(text, heading ? button_font : button_font_small);
    caption.setZOrdering(4);
    caption.updateEngineTextObject();
    if(caption.getSizeX() > 900.0f) {
        FontSetup font = heading ? button_font : button_font_small;
        font.size = max(20, int(float(font.size) * 900.0f / caption.getSizeX()));
        caption.setFont(font);
    }
    label.setElement(caption);
    parent.append(label);
}

void PlayerTeamsNote(const string &in text, IMDivider@ parent) {
    IMContainer label(900.0f, 40.0f);
    label.setAlignment(CALeft, CACenter);
    IMText caption(text, button_font_extra_small);
    caption.setZOrdering(4);
    label.setElement(caption);
    parent.append(label);
}

void BuildPlayerTeamsMenu() {
    ClearControllerItems();
    imGUI.clear();
    imGUI.setup();
    gui_elements.resize(0);
    category_elements.resize(0);
    active_slider = -1;
    personal_settings = false;
    personal_teams = true;
    personal_bot_name = false;
    capture_input = false;
    player_menu_revision = LobbySnapshotRevision();
    array<LobbyPlayer> players = LobbyPlayers();
    team_scroll_offset = max(0, min(team_scroll_offset, int(players.length()) - kVisibleTeamRows));

    IMContainer panel(1100.0f, 1300.0f);
    panel.setAlignment(CACenter, CACenter);
    IMImage background("Textures/ui/menus/main/inGameMenu-bg.png");
    background.setSize(vec2(1100.0f, 1300.0f));
    background.setColor(vec4(0, 0, 0, 0.9f));
    background.setZOrdering(0);
    panel.addFloatingElement(background, "teams_background", vec2(0), 0);
    IMDivider content("teams_content", DOVertical);
    content.setAlignment(CACenter, CACenter);
    panel.setElement(content);
    PlayerTeamsLabel("Teams and screens", content, true);
    PlayerTeamsNote(LobbyGetHostControlsTeams() ? "The host assigns teams." : "Players choose their own team in lobby-team modes.", content);
    if(!LobbyGetForceTeams()) PlayerTeamsNote("The selected mode may manage its own teams.", content);
    PlayerTeamsNote("The local leader assigns this couch's screens.", content);
    content.appendSpacer(30.0f);
    IMDivider list_and_scroll("teams_scroll_region", DOHorizontal);
    IMDivider rows("teams_rows", DOVertical);
    rows.setSizeX(900.0f);
    rows.setSizeY(840.0f);
    rows.setAlignment(CALeft, CATop);
    list_and_scroll.append(rows);
    int local_count = 0;
    for(uint i = 0; i < players.length(); ++i) if(players[i].local && !players[i].bot) ++local_count;
    for(int row = team_scroll_offset; row < min(int(players.length()), team_scroll_offset + kVisibleTeamRows); ++row) {
        LobbyPlayer player = players[row];
        IMDivider row_content("team_row_" + row, DOVertical);
        row_content.setAlignment(CALeft, CATop);
        uint controls_before = controller_items.length();
        string name = player.name;
        if(player.bot) name += " (bot)";
        if(player.host) name += " (host)";
        if(player.leader) name += " (local leader)";
        PlayerTeamsLabel(name, row_content);
        if(LobbyCanSetPlayerTeam(player.id)) {
            AddDropDown("Player " + (row + 1), row_content, DropdownConfiguration(PlayerTeamConfiguration(player.id, int(players.length()))));
        } else {
            PlayerTeamsNote("Team " + player.team, row_content);
        }
        string affiliation = LobbyCampaignTeamAffiliation(player.lobby_team);
        if(affiliation != "") PlayerTeamsNote(affiliation, row_content);
        if(player.bot) {
            if(LobbyIsHost()) {
                IMDivider actions("bot_actions_" + player.id, DOHorizontal);
                if(LobbyCanRenamePlayer(player.id))
                    AddButton("Rename", actions, "null", button_background_diamond, false, 300.0f,
                              10.0f, mouseover_scale_button, IMMessage("rename_bot", int(player.id)));
                if(LobbyCanRemovePlayer(player.id))
                    AddButton("Remove", actions, "null", button_background_diamond, false, 300.0f,
                              10.0f, mouseover_scale_button, IMMessage("remove_bot", int(player.id)));
                row_content.append(actions);
            }
        } else if(player.local && LobbyCanManageLocal()) {
            AddDropDown("Screen P" + (player.local_index + 1), row_content,
                        DropdownConfiguration(PlayerScreenConfiguration(player.id, local_count)));
        } else {
            PlayerTeamsNote(player.local ? "Screen " + player.screen : "Screen: remote client", row_content);
        }
        if(controller_items.length() == controls_before) AddControllerItem(row_content, IMMessage("controller_row"));
        row_content.appendSpacer(15.0f);
        rows.append(row_content);
    }
    AddScrollBar(list_and_scroll, 60.0f, 840.0f, int(players.length()), kVisibleTeamRows, team_scroll_offset);
    ConfigureControllerScrollRegion(rows, team_scroll_offset, kVisibleTeamRows, int(players.length()));
    content.append(list_and_scroll);
    content.appendSpacer(30.0f);
    AddButton("Back", content, arrow_icon, button_back, true, 350.0f, 10.0f);
    imGUI.getMain().setElement(panel);
    controller_wraparound = false;
}

void ProcessPlayerTeamsMessage(IMMessage@ message) {
    if(message.name == "Back" || message.name == "back") MenuBack();
    else if(message.name == "controller_row") {} // Read-only rows remain reachable for scrolling.
    else if(message.name == "rename_bot") OpenBotNameMenu(uint(message.getInt(0)));
    else if(message.name == "remove_bot") {
        if(LobbyIsHost()) LobbyRemovePlayer(uint(message.getInt(0)));
        BuildPlayerTeamsMenu();
    }
    else if(message.name == "shift_menu" || message.name == "controller_scroll") {
        team_scroll_offset += message.getInt(0);
        BuildPlayerTeamsMenu();
        if(message.name == "controller_scroll") FocusControllerScrollEdge(controller_scroll_region, message.getInt(0));
    } else if(message.name == "ui_element_clicked") {
        CloseAllOptionMenus();
        ToggleUIElement(message.getString(0));
    } else if(message.name == "option_changed") {
        for(uint i = 0; i < gui_elements.size(); ++i)
            if(gui_elements[i].name == message.getString(0)) gui_elements[i].SwitchOption(message.getString(1));
    } else if(message.name == "slider_activate") {
        old_mouse_pos = imGUI.guistate.mousePosition;
        active_slider = message.getInt(0);
    } else if(message.name == "slider_deactivate") {
        if(!checking_slider_movement) active_slider = -1;
    } else if(message.name == "close_all_open_menus") CloseAllOptionMenus();
}

void OpenBotNameMenu(uint id) {
    if(!LobbyCanRenamePlayer(id)) return;
    array<LobbyPlayer> players = LobbyPlayers();
    for(uint i = 0; i < players.length(); ++i) {
        if(players[i].id == id && players[i].bot) {
            editing_bot = int(id);
            bot_name_editor.input_text = players[i].name;
            bot_name_editor.cursor_offset = 0;
            bot_name_notice = "";
            BuildBotNameMenu();
            return;
        }
    }
}

void BotNameCharacter(const string &in character) {
    if(bot_name_editor.input_text.length() < 64) bot_name_editor.input_text += character;
    bot_name_editor.cursor_offset = 0;
    bot_name_editor.Deactivate();
    capture_input = false;
    bot_name_editor.UpdateText();
}

void BuildBotNameMenu() {
    ClearControllerItems();
    imGUI.clear();
    imGUI.setup();
    gui_elements.resize(0);
    category_elements.resize(0);
    active_slider = -1;
    personal_settings = false;
    personal_teams = false;
    personal_bot_name = true;
    capture_input = false;
    bot_name_editor.Deactivate();
    bot_name_editor.max_length = 64;
    bot_name_editor.on_enter_message = "save_bot_name";
    player_menu_revision = LobbySnapshotRevision();
    IMContainer panel(1100.0f, 1200.0f);
    panel.setAlignment(CACenter, CACenter);
    IMImage background(white_background);
    background.setSize(vec2(1100.0f, 1200.0f));
    background.setColor(vec4(0, 0, 0, 0.95f));
    background.setZOrdering(0);
    panel.addFloatingElement(background, "bot_name_background", vec2(0), 0);
    IMDivider content("bot_name_content", DOVertical);
    content.setAlignment(CACenter, CACenter);
    panel.setElement(content);
    PlayerTeamsLabel("Name this bot", content, true);
    AddTextInput(content, bot_name_editor, "activate_bot_name");
    PlayerTeamsNote(MenuOwnsKeyboard() ? "Type a name or select letters below." : "Select letters with your controller.", content);
    content.appendSpacer(25.0f);
    const string letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    for(uint row = 0; row < (letters.length() + 7) / 8; ++row) {
        IMDivider keys("name_keys_" + row, DOHorizontal);
        for(uint column = 0; column < 8; ++column) {
            uint index = row * 8 + column;
            if(index >= letters.length()) break;
            string letter = letters.substr(index, 1);
            AddButton(letter, keys, "null", button_background_diamond, false, 90.0f,
                      0.0f, mouseover_scale_button, IMMessage("bot_name_character", letter));
        }
        content.append(keys);
    }
    IMDivider edit_actions("bot_name_edit_actions", DOHorizontal);
    AddButton("Space", edit_actions, "null", button_background_diamond, false, 300.0f,
              10.0f, mouseover_scale_button, IMMessage("bot_name_space"));
    AddButton("Erase", edit_actions, "null", button_background_diamond, false, 300.0f,
              10.0f, mouseover_scale_button, IMMessage("bot_name_erase"));
    content.append(edit_actions);
    IMDivider actions("bot_name_actions", DOHorizontal);
    AddButton("Save", actions, "null", button_background_diamond, false, 300.0f,
              10.0f, mouseover_scale_button, IMMessage("save_bot_name"));
    AddButton("Cancel", actions, "null", button_background_diamond, false, 300.0f,
              10.0f, mouseover_scale_button, IMMessage("cancel_bot_name"));
    content.append(actions);
    if(bot_name_notice != "") PlayerTeamsNote(bot_name_notice, content);
    imGUI.getMain().setElement(panel);
    controller_wraparound = false;
}

void ProcessBotNameMessage(IMMessage@ message) {
    if(message.name == "cancel_bot_name" || message.name == "Back") BuildPlayerTeamsMenu();
    else if(message.name == "activate_bot_name") {
        if(MenuOwnsKeyboard()) {
            bot_name_editor.Activate();
            capture_input = true;
        }
    } else if(message.name == "bot_name_character") BotNameCharacter(message.getString(0));
    else if(message.name == "bot_name_space") BotNameCharacter(" ");
    else if(message.name == "bot_name_erase") {
        bot_name_editor.cursor_offset = 0;
        bot_name_editor.InputBackspace();
        bot_name_editor.Deactivate();
        capture_input = false;
    } else if(message.name == "save_bot_name") {
        if(LobbyCanRenamePlayer(uint(editing_bot)) && LobbySetPlayerName(uint(editing_bot), bot_name_editor.GetText())) {
            bot_name_editor.Deactivate();
            BuildPlayerTeamsMenu();
        } else {
            bot_name_notice = "Enter a valid name; only the host may name bots.";
            BuildBotNameMenu();
        }
    }
}

void Update() {
    if(save_cards_open) UpdateSaveCards();
    if(!save_cards_open && !personal_settings && !personal_teams && !personal_bot_name &&
       (world_paused != GetPlayerMenuWorldPaused() || player_menu_revision != LobbySnapshotRevision())) {
        BuildPlayerPauseMenu();
    }
    if(personal_settings) UpdateSettings(false);
    if(personal_bot_name) {
        if(editing_bot < 0 || !LobbyCanRenamePlayer(uint(editing_bot))) BuildPlayerTeamsMenu();
        else bot_name_editor.Update();
    }
    if(personal_teams) {
        if(player_menu_revision != LobbySnapshotRevision() && !OptionMenuOpen()) BuildPlayerTeamsMenu();
        UpdateMovingSlider();
        if(MenuOwnsKeyboard() && GetInputPressed(controller_player, "mousescrollup")) imGUI.receiveMessage(IMMessage("shift_menu", -1));
        if(MenuOwnsKeyboard() && GetInputPressed(controller_player, "mousescrolldown")) imGUI.receiveMessage(IMMessage("shift_menu", 1));
    }
    while(imGUI.getMessageQueueSize() > 0) {
        IMMessage@ message = imGUI.getNextMessage();
        if(message.name == "open_save_cards" || message.name == "open_load_cards") {
            OpenSaveCards(message.name == "open_save_cards"); continue;
        }
        if(save_cards_open) {
            if(message.name=="save_back" || message.name=="Back") BuildPlayerPauseMenu();
            else SaveCardMessage(message);
            continue;
        }
        if(message.name == "capture_input") {
            capture_input = true;
        } else if(message.name == "release_input") {
            capture_input = false;
        } else if(personal_bot_name) {
            ProcessBotNameMessage(message);
        } else if(personal_teams) {
            ProcessPlayerTeamsMessage(message);
        } else if(message.name == "Back") {
            BuildPlayerPauseMenu();
        } else if(message.name == "rebuild_settings" || message.name == "Settings_refresh") {
            if(personal_settings) {
                string page = current_screen;
                ClearControllerItems();
                BuildUI(page);
            }
        } else if(personal_settings) {
            ProcessSettingsMessage(message);
        } else if(message.name == "Continue") {
            this_ui.SendCallback("close");
        } else if(message.name == "Settings") {
            OpenPersonalSettings();
        } else if(message.name == "Teams") {
            BuildPlayerTeamsMenu();
        } else if(message.name == "open_party_online") {
            local_player_notice = LobbyOpenToNetwork() ? "Online hosting enabled" : "Could not enable online hosting";
            BuildPlayerPauseMenu();
        } else if(message.name == "toggle_world_pause") {
            this_ui.SendCallback("toggle_pause");
        } else if(message.name == "Retry") {
            this_ui.SendCallback("retry");
        } else if(message.name == "end_game_to_lobby") {
            this_ui.SendCallback("end_game");
        } else if(message.name == "Main Menu") {
            this_ui.SendCallback("main_menu");
        } else if(message.name == "Invite friends") {
            if(LobbyIsHost()) Online_ActivateInviteDialog();
        } else if(message.name == "Media Mode") {
            if(LobbyCanManageLocal()) {
                this_ui.SendCallback("media_mode");
            }
        } else if(message.name == "add_lobby_bot") {
            if(LobbyIsHost()) {
                local_player_notice = LobbyAddBot() ? "" : "Could not add slot";
                BuildPlayerPauseMenu();
            }
        }
    }
    imGUI.update();
    if(!capture_input) UpdateController();
    if(GetInputPressed(controller_player, "menu_cancel")) MenuBack();
}

void Resize() {
    imGUI.doScreenResize();
    if(save_cards_open) { BuildSaveCards(); return; }
    if(personal_settings) {
        string page = current_screen;
        ClearControllerItems();
        BuildUI(page);
    } else if(personal_bot_name) {
        BuildBotNameMenu();
    } else if(personal_teams) {
        BuildPlayerTeamsMenu();
    } else {
        BuildPlayerPauseMenu();
    }
}

void ScriptReloaded() { Dispose(); Initialize(); }
void DrawGUI() { imGUI.render(); }
void Draw() {}
void Init(string str) {}
