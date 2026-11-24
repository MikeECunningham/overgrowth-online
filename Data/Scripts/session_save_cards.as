// Shared by the main menu, the lobby and each player's pause context.
bool save_cards_open = false;
bool saving_cards = false;
int save_page = 0;
int overwrite_slot = -1;
int writing_slot = -1;
FontSetup card_info_font("OpenSans-Regular", 34, vec4(0.94f,0.92f,0.89f,1), true);
FontSetup card_date_font("OpenSans-Regular", 31, vec4(0.81f,0.79f,0.77f,1), true);
string save_notice;

string CardTitle(int slot) { return slot == 0 ? "Autosave" : "Slot " + slot; }

void BuildSaveCards() {
    ClearControllerItems();
    imGUI.clear();
    imGUI.setup();
    IMDivider page(DOVertical);
    page.setAlignment(CACenter, CACenter);
    page.append(IMText(saving_cards ? "Save game" : "Load game", button_font));
    page.appendSpacer(35);
    IMDivider row(DOHorizontal);
    row.setAlignment(CACenter, CATop);
    int first = (saving_cards ? 1 : 0) + save_page * 3;
    for(int slot=first; slot<first+3 && slot<=6; ++slot) {
        bool occupied = SessionSaveExists(slot);
        IMContainer tile(570, 525);
        tile.setAlignment(CACenter, CATop);
        IMImage backdrop(white_background);
        backdrop.setSize(vec2(570,525));
        backdrop.setColor(vec4(0.035f,0.03f,0.025f,0.88f));
        tile.addFloatingElement(backdrop, "slot_background", vec2(0), 1);
        IMDivider content(DOVertical);
        content.setZOrdering(4);
        content.setAlignment(CACenter, CATop);
        content.appendSpacer(14);
        content.append(IMText(CardTitle(slot),button_font_small));
        content.appendSpacer(10);
        if(occupied) {
            IMImage picture(SessionSaveCard(slot,"image"));
            picture.setSize(vec2(448,252));
            content.append(picture);
            content.appendSpacer(12);
            content.append(IMText(SessionSaveCard(slot,"mode"),card_info_font));
            content.append(IMText(SessionSaveCard(slot,"map"),card_info_font));
            content.append(IMText(SessionSaveCard(slot,"date"),card_date_font));
        } else {
            content.appendSpacer(104);
            content.append(IMText("Empty",button_font_small));
        }
        tile.setElement(content);
        if((occupied || saving_cards) && writing_slot < 0) {
            IMMessage action("choose_save_card",slot);
            tile.addLeftMouseClickBehavior(IMFixedMessageOnClick(action),"");
            tile.addMouseOverBehavior(mouseover_scale_button,"");
            AddControllerItem(tile,action);
        }
        row.append(tile);
        if(slot < first+2 && slot<6) row.appendSpacer(20);
    }
    page.append(row);
    if(save_notice != "") { page.appendSpacer(12); page.append(IMText(save_notice,button_font_extra_small)); }
    IMDivider actions(DOHorizontal);
    if(overwrite_slot >= 0) {
        AddButton("Replace " + CardTitle(overwrite_slot), actions, "null", button_background_diamond,false,520,15,mouseover_scale_button,IMMessage("confirm_save"));
        AddButton("Cancel", actions,"null",button_background_diamond,false,350,15,mouseover_scale_button,IMMessage("cancel_save"));
    } else {
        if(save_page > 0) AddButton("Previous",actions,"null",button_background_diamond,false,425,15,mouseover_scale_button,IMMessage("save_prev"));
        if(first+3 <= 6) AddButton("Next",actions,"null",button_background_diamond,false,375,15,mouseover_scale_button,IMMessage("save_next"));
        AddButton("Back",actions,"null",button_background_diamond,false,375,15,mouseover_scale_button,IMMessage("save_back"));
    }
    page.appendSpacer(24);
    page.append(actions);
    imGUI.getMain().setAlignment(CACenter,CACenter);
    imGUI.getMain().setElement(page);
    controller_wraparound=true;
}
void OpenSaveCards(bool writing) {
    save_cards_open = true;
    saving_cards = writing;
    save_page = 0;
    overwrite_slot = -1;
    writing_slot = -1;
    save_notice = "";
    BuildSaveCards();
}

void UpdateSaveCards() {
    if(writing_slot >= 0 && SessionSaveStatus() != save_notice) {
        writing_slot = -1;
        save_notice = SessionSaveStatus();
        BuildSaveCards();
    }
}

void ChooseSaveCard(int slot) {
    if(saving_cards) {
        if(SessionSave(slot)) writing_slot = slot;
    } else if(SessionLoad(slot)) {
        return;
    }
    save_notice = SessionSaveStatus();
    BuildSaveCards();
}

void SaveCardMessage(IMMessage@ msg) {
    if(msg.name == "save_next" && save_page < (saving_cards ? 1 : 2)) {
        ++save_page;
    } else if(msg.name == "save_prev" && save_page > 0) {
        --save_page;
    } else if(msg.name == "cancel_save") {
        overwrite_slot = -1;
        save_notice = "";
    } else if(msg.name == "confirm_save" && overwrite_slot >= 0) {
        int slot = overwrite_slot;
        overwrite_slot = -1;
        ChooseSaveCard(slot);
        return;
    } else if(msg.name == "choose_save_card" && overwrite_slot < 0 && writing_slot < 0) {
        int slot = msg.getInt(0);
        if(saving_cards && SessionSaveExists(slot)) {
            overwrite_slot = slot;
            save_notice = "Replace this save?";
        } else {
            ChooseSaveCard(slot);
            return;
        }
    } else {
        return;
    }
    BuildSaveCards();
}
