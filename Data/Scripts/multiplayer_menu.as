// Multiplayer starts with the local client's role. The host chooses the game
// after entering a lobby; joining never loads a level before that host starts.
// License: Apache-2.0, like the rest of the engine.
#include "menu_common.as"
#include "multiplayer_menu_style.as"
#include "music_load.as"

MusicLoad ml("Data/Music/menu.xml");
IMGUI@ imGUI;
string notice;
bool focus_host = true;

bool HasFocus() { return false; }
bool CanGoBack() {
    if(HasPopup()) {
        RemovePopup();
        return false;
    }
    return true;
}

void Initialize() {
    ClearControllerItems();
    SetControllerPlayer(0);
    focus_host = true;
    @imGUI = CreateIMGUI();
    imGUI.setup();
    setBackGround();
    PlaySong("overgrowth_main");
    if(LobbyActive()) {
        this_ui.SendCallback("multiplayer_lobby.as");
    }
}

void Dispose() {
    ClearControllerItems();
    imGUI.clear();
}

void Update() {
    if(HasPopup()) {
        UpdateController();
        imGUI.update();
        return;
    }
    UpdateKeyboardMouse();
    imGUI.update();
    if(GetInputPressed(0, "menu_cancel")) {
        this_ui.SendCallback("back");
    }
}

void Resize() { imGUI.doScreenResize(); setBackGround(); }
void ScriptReloaded() { Dispose(); Initialize(); }
void Draw() {}
void Init(string str) {}

void DrawGUI() {
    imGUI.render();
    if(HasPopup()) {
        return;
    }
    MPStyleBegin();
    float sw = float(GetScreenWidth());
    float sh = float(GetScreenHeight());
    float width = min(920.0f, sw - 48.0f);
    float scale = sh >= 1000.0f ? 1.2f : 1.0f;
    ImGui_SetNextWindowPos(vec2((sw - width) * 0.5f, sh * 0.22f), ImGuiSetCond_Always);
    ImGui_SetNextWindowSize(vec2(width, 0.0f), ImGuiSetCond_Always);
    ImGui_PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(24.0f, 24.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(16.0f, 14.0f));
    ImGui_Begin("Multiplayer", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    MPWindowScale(scale);
    MPPanelBackground();
    MPHeading("Multiplayer", 54.0f);
    ImGui_TextWrapped("Host a game for your couch and online friends, or connect to another host.");
    ImGui_Spacing();
    float card_width = (ImGui_GetContentRegionAvailWidth() - 16.0f) * 0.5f;
    if(focus_host) {
        ImGui_SetWindowFocus();
    }
    if(MPButton("Host\nCreate a lobby", vec2(card_width, 104.0f * scale))) {
        if(LobbyBeginHost()) {
            this_ui.SendCallback("multiplayer_lobby.as");
        } else {
            notice = "Couldn't open a lobby. Check the multiplayer port and try again.";
        }
    }
    ImGui_SetItemDefaultFocus();
    if(focus_host) {
        ImGui_SetKeyboardFocusHere(-1);
        focus_host = false;
    }
    ImGui_SameLine();
    if(MPButton("Connect\nJoin a host", vec2(card_width, 104.0f * scale))) {
        this_ui.SendCallback("multiplayer_connect.as");
    }
    if(notice != "") {
        ImGui_TextColored(vec4(1.0f, 0.65f, 0.4f, 1.0f), notice);
    }
    ImGui_Spacing();
    ImGui_Separator();
    if(MPButton("Back", vec2(140.0f * scale, 36.0f * scale))) {
        this_ui.SendCallback("back");
    }
    ImGui_End();
    ImGui_PopStyleVar(2);
    MPStyleEnd();
}
