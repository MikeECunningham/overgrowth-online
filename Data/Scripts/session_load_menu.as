#include "menu_common.as"
#include "session_save_cards.as"

IMGUI@ imGUI;

bool HasFocus() { return false; }
bool CanGoBack() { return true; }

void Initialize() {
    @imGUI = CreateIMGUI();
    OpenSaveCards(false);
    setBackGround();
}

void Dispose() {
    ClearControllerItems();
    imGUI.clear();
}

void Update() {
    while(imGUI.getMessageQueueSize() > 0) {
        IMMessage@ msg = imGUI.getNextMessage();
        if(msg.name == "save_back") this_ui.SendCallback("back");
        else SaveCardMessage(msg);
    }
    UpdateController();
    imGUI.update();
}

void Resize() {
    imGUI.doScreenResize();
    BuildSaveCards();
    setBackGround();
}

void DrawGUI() { imGUI.render(); }
void Draw() {}
void Init(string str) {}
void ScriptReloaded() { Initialize(); }
