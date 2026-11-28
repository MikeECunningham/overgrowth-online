// Connection methods share one form; role selection lives one step above it.
// License: Apache-2.0.
#include "menu_common.as"
#include "multiplayer_menu_style.as"
#include "music_load.as"

MusicLoad ml("Data/Music/menu.xml");
IMGUI@ imGUI;
string address;
string notice;
bool focus_connect = true;

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
    focus_connect = true;
    SetControllerPlayer(0);
    @imGUI = CreateIMGUI();
    imGUI.setup();
    setBackGround();
    PlaySong("overgrowth_main");
    address = GetConfigValueString("last_ip");
}

void Dispose() { ClearControllerItems(); imGUI.clear(); }
void Update() {
    if(HasPopup()) {
        UpdateController();
        imGUI.update();
        return;
    }
    UpdateKeyboardMouse();
    imGUI.update();
    if(GetInputPressed(0, "menu_cancel") && !ImGui_IsAnyItemActive()) {
        this_ui.SendCallback("back");
    }
}
void Resize() { imGUI.doScreenResize(); setBackGround(); }
void ScriptReloaded() { Dispose(); Initialize(); }
void Draw() {}
void Init(string str) {}

void Connect(bool keyboard_submit) {
    if(address == "") {
        notice = "Enter the host's address first.";
        return;
    }
    SetConfigValueString("last_ip", address);
    SaveConfig();
    LobbyCaptureRoleDevice(true, keyboard_submit);
    Online_Connect(address);
    this_ui.SendCallback("multiplayer_menu_connecting.as");
}

void DrawGUI() {
    imGUI.render();
    if(HasPopup()) {
        return;
    }
    MPStyleBegin();
    float sw = float(GetScreenWidth());
    float sh = float(GetScreenHeight());
    float width = min(760.0f, sw - 48.0f);
    float scale = sh >= 1000.0f ? 1.2f : 1.0f;
    ImGui_SetNextWindowPos(vec2((sw - width) * 0.5f, sh * 0.16f), ImGuiSetCond_Always);
    ImGui_SetNextWindowSize(vec2(width, 0.0f), ImGuiSetCond_Always);
    ImGui_PushStyleVar(ImGuiStyleVar_WindowPadding, vec2(24.0f, 24.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(12.0f, 14.0f));
    ImGui_Begin("Connect to a host", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
    if(focus_connect) ImGui_SetWindowFocus();
    MPWindowScale(scale);
    MPPanelBackground();
    MPHeading("Connect to a host", 50.0f);
    ImGui_TextWrapped("Join the host's lobby, then add your local players before the game starts.");
    ImGui_Spacing();
    ImGui_Text("Direct IP");
    ImGui_PushItemWidth(-1.0f);
    bool enter = ImGui_InputText("##host_address", address, 256, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui_PopItemWidth();
    if(MPButton("Connect", vec2(-1.0f, 40.0f * scale)) || enter) {
        Connect(enter);
    }
    if(focus_connect) {
        ImGui_SetItemDefaultFocus();
        ImGui_SetKeyboardFocusHere(-1);
        focus_connect = false;
    }
    if(notice != "") {
        ImGui_TextColored(vec4(1.0f, 0.65f, 0.4f, 1.0f), notice);
    }
    ImGui_Separator();
    float half = (ImGui_GetContentRegionAvailWidth() - 12.0f) * 0.5f;
    if(MPButton("Browse games", vec2(half, 52.0f * scale))) {
        this_ui.SendCallback("multiplayer_browser.as");
    }
    ImGui_SameLine();
    if(MPButton("Join a Steam friend", vec2(half, 52.0f * scale))) {
        this_ui.SendCallback("multiplayer_join_friend.as");
    }
    ImGui_TextWrapped("Steam friends and the public browser require a Steamworks build. Direct IP works without Steam.");
    ImGui_Separator();
    if(MPButton("Back", vec2(140.0f * scale, 36.0f * scale))) {
        this_ui.SendCallback("back");
    }
    ImGui_End();
    ImGui_PopStyleVar(2);
    MPStyleEnd();
}
