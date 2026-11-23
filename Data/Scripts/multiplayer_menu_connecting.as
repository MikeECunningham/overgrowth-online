//-----------------------------------------------------------------------------
//           Name: multiplayer_menu_connecting.as
//      Developer: Wolfire Games LLC
//    Script Type:
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
//   Copyright 2022 Wolfire Games LLC
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//-----------------------------------------------------------------------------

#include "save/general.as"
#include "menu_common.as"
#include "music_load.as"

MusicLoad ml("Data/Music/menu.xml");

IMGUI@ imGUI;

bool back_event_queued = false;
bool cancel_requested = false;
float dot_counter = 0;
IMText@ connecting_text;

// Overgrowth Online: Cancel, and no endless wait: an answer comes within
// kGiveUpSeconds or the attempt ends with a word on why
const float kGiveUpSeconds = 20.0f;
float waited = 0.0f;
float leave_at = -1.0f;       // showing why it failed until then
FontSetup reason_font("edosz", 55, HexColor("#E0B060"), true);
IMText@ reason_text;

bool HasFocus() {
    return false;
}

void Initialize() {
    ClearControllerItems();
    SetControllerPlayer(0);
    back_event_queued = false;
    cancel_requested = false;
    waited = 0.0f;
    leave_at = -1.0f;
    @imGUI = CreateIMGUI();

    imGUI.setHeaderHeight(0);
    imGUI.setFooterHeight(0);
    imGUI.setup();

    BuildUI();
    setBackGround();
}

void BuildUI() {
    IMImage dim(white_background);
    dim.setSize(imGUI.getMain().getSize());
    dim.setColor(vec4(0, 0, 0, 0.65f));
    dim.setZOrdering(0);
    imGUI.getMain().addFloatingElement(dim, "connecting_backdrop", vec2(0), 0);

    IMContainer panel(1400.0f, 650.0f);
    panel.setAlignment(CACenter, CACenter);
    IMImage background(white_background);
    background.setSize(vec2(1400.0f, 650.0f));
    background.setColor(vec4(0.025f, 0.025f, 0.025f, 0.98f));
    background.setZOrdering(0);
    panel.addFloatingElement(background, "connecting_panel_background", vec2(0), 0);
    IMDivider column("column", DOVertical);
    column.setAlignment(CACenter, CACenter);
    panel.setElement(column);
    IMContainer heading(1200.0f, 128.0f);
    heading.setAlignment(CACenter, CACenter);
    IMImage brush(brushstroke_background);
    brush.setSize(vec2(1200.0f, 128.0f));
    heading.addFloatingElement(brush, "connecting_brush", vec2(0), 1);
    IMText title("Connecting", button_font);
    title.setZOrdering(2);
    heading.setElement(title);
    column.append(heading);
    column.appendSpacer(35.0f);

    @connecting_text = IMText("Waiting for the host...", button_font_small);
    connecting_text.setZOrdering(4);
    IMContainer text_row(1250.0f, 80.0f);
    text_row.setAlignment(CACenter, CACenter);
    text_row.setElement(connecting_text);
    column.append(text_row);

    @reason_text = IMText("", reason_font);
    reason_text.setZOrdering(4);
    IMContainer reason_row(1250.0f, 120.0f);
    reason_row.setAlignment(CACenter, CACenter);
    reason_row.setElement(reason_text);
    column.append(reason_row);
    column.append(IMSpacer(DOVertical, 40));

    // (Overgrowth Online: closing a connection still being made works now)
    AddButton("Cancel", column, exit_icon, button_background_diamond, true, 650.0f);
    imGUI.getMain().addFloatingElement(panel, "connecting_dialog", vec2(UNDEFINEDSIZE, UNDEFINEDSIZE), 1);
}

void Dispose() {
    ClearControllerItems();
    imGUI.clear();
}

bool CanGoBack() {
    CancelConnection();
    return true;
}

void CancelConnection() {
    if(!back_event_queued) Online_Close();
    cancel_requested = true;
    back_event_queued = true;
}

void Update() {
    UpdateController();
    UpdateKeyboardMouse();
    imGUI.update();
    // Handle cancellation before checking handshake completion. A Cancel that
    // was clicked this frame must not disappear behind a menu transition.
    while(imGUI.getMessageQueueSize() > 0) {
        IMMessage@ message = imGUI.getNextMessage();
        if(message.name == "Cancel") CancelConnection();
    }
    if(GetInputPressed(controller_player, "menu_cancel")) CancelConnection();
    if(cancel_requested || (back_event_queued && !HasPopup())) {
        this_ui.SendCallback("back");
        return;
    }
    LobbyUpdate();
    if(LobbyActive() && leave_at < 0.0f && !back_event_queued) {
        this_ui.SendCallback("multiplayer_lobby.as");
        return;
    }

    // Animate connecting text
    if(leave_at < 0.0f) {
        UpdateConnectingText();
    }

    // No answer in time: stop trying, say so, then back
    waited += time_step;
    if(leave_at < 0.0f && waited > kGiveUpSeconds && !back_event_queued) {
        Online_Close();
        connecting_text.setText("No answer");
        reason_text.setText("The host didn't answer.\nCheck the address and that their game is open.");
        leave_at = waited + 3.0f;
    }
    if(leave_at >= 0.0f && waited > leave_at) {
        back_event_queued = true;
    }

    // This is a weak check for connection failures.
    // We expect to either move out of this menu on success, or fail connecting with a popup message
    // If the popup doesn't show up for whatever reason, we might end up with a softlock
    if(HasPopup()) {
        back_event_queued = true;
    } else if(back_event_queued) {
        this_ui.SendCallback("back");
    }

}

void UpdateConnectingText() {
    dot_counter += time_step * 2;
    uint count = uint(dot_counter % 3 + 1);

    string dots = "";
    for(uint i = 0; i < count; i++) {
        dots += ".";
    }
    connecting_text.setText("Waiting for the host" + dots);
}

void Resize() {
    imGUI.doScreenResize(); // This must be called first
    setBackGround();
}

void ScriptReloaded() {
    // Clear the old GUI
    imGUI.clear();
    // Rebuild it
    Initialize();
}

void DrawGUI() {
    imGUI.render();
}

void Draw() {

}

void Init(string str) {

}
