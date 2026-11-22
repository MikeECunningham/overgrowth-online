// Each local player owns a separate instance of this script and settings tree.
// The native menu manager supplies that player's input and camera viewport.
#include "menu_common.as"
#include "settings.as"

IMGUI@ imGUI;
bool capture_input = false;
bool personal_settings = false;
bool world_paused = false;
string local_player_notice;

bool HasFocus() { return capture_input; }

void MenuBack() {
    if(HasPopup()) {
        RemovePopup();
    } else if(personal_settings) {
        ProcessSettingsMessage(IMMessage("back"));
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
    ClearControllerItems();
    imGUI.clear();
    imGUI.setup();
    gui_elements.resize(0);
    category_elements.resize(0);
    personal_settings = false;
    capture_input = false;
    world_paused = GetPlayerMenuWorldPaused();
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

    const bool primary = GetPlayerMenuLocalIndex() == 0;
    AddButton("Continue", buttons, "null", button_background_diamond, false, 650.0f,
              30.0f, mouseover_scale_button, IMMessage("Continue"));
    AddButton("Settings", buttons, "null", button_background_diamond, false, 650.0f,
              30.0f, mouseover_scale_button, IMMessage("Settings"));
    if((Online_IsActive() || GetLocalPlayerCount() > 1) && LobbyCanPause()) {
        AddButton(world_paused ? "Resume world" : "Pause world", buttons, "null", button_background_diamond,
                  false, 650.0f, 30.0f, mouseover_scale_button, IMMessage("toggle_world_pause"));
    }
    if(primary && !Online_IsClient()) {
        AddButton("Retry", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Retry"));
    }
    if(primary && Online_IsHosting()) {
        AddButton("End game", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("end_game_to_lobby"));
    }
    if(primary && Online_IsClient()) {
        AddButton("Disconnect", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Main Menu"));
    }
    if(primary && Online_IsHosting() && Online_HasFriendInviteOverlay()) {
        AddButton("Invite friends", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("Invite friends"));
    }
    if(primary && !Online_IsActive()) {
        AddButton("Add player (debug)", buttons, "null", button_background_diamond, false, 650.0f,
                  30.0f, mouseover_scale_button, IMMessage("debug_add_local_player"));
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
    personal_settings = true;
    capture_input = false;
    SetControllerPlayer(GetPlayerMenuInputID());
    BuildUI(MenuOwnsKeyboard() ? "Controls" : "Gamepad");
}

void Update() {
    if(!personal_settings && world_paused != GetPlayerMenuWorldPaused()) {
        BuildPlayerPauseMenu();
    }
    if(personal_settings) UpdateSettings(false);
    while(imGUI.getMessageQueueSize() > 0) {
        IMMessage@ message = imGUI.getNextMessage();
        if(message.name == "capture_input") {
            capture_input = true;
        } else if(message.name == "release_input") {
            capture_input = false;
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
        } else if(message.name == "toggle_world_pause") {
            this_ui.SendCallback("toggle_pause");
        } else if(message.name == "Retry") {
            this_ui.SendCallback("retry");
        } else if(message.name == "end_game_to_lobby") {
            this_ui.SendCallback("end_game");
        } else if(message.name == "Main Menu") {
            this_ui.SendCallback("main_menu");
        } else if(message.name == "Invite friends") {
            if(GetPlayerMenuLocalIndex() == 0) Online_ActivateInviteDialog();
        } else if(message.name == "Media Mode") {
            if(GetPlayerMenuLocalIndex() == 0) {
                this_ui.SendCallback("media_mode");
            }
        } else if(message.name == "debug_add_local_player") {
            if(GetPlayerMenuLocalIndex() == 0) {
                local_player_notice = DebugAddLocalPlayer() < 0 ? "Could not add player" : "";
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
    if(personal_settings) {
        string page = current_screen;
        ClearControllerItems();
        BuildUI(page);
    } else {
        BuildPlayerPauseMenu();
    }
}

void ScriptReloaded() { Dispose(); Initialize(); }
void DrawGUI() { imGUI.render(); }
void Draw() {}
void Init(string str) {}
