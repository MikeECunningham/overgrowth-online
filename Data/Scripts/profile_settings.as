#include "profile_gui_common.as"

class ProfileNameValidator : TextValidator {
    bool IsLegalKeycode(int code) override { return ProfileNameCharacter(code); }
}

string settings_profile;
string settings_profile_category;
int settings_skin_page = 0;
string profile_editor_mode, profile_notice;
TextBox profile_name_box;
int settings_mod_tab = 0, settings_mod_page = 0;
string settings_choices_field;
int settings_choices_page = 0;
uint profile_settings_revision;

JSONValue ReadProfileJSON(const string &in value) {
    JSON json;
    json.parseString(value);
    return json.getRoot();
}

// A bounded dropdown keeps every entry reachable on the fixed-size Settings canvas.
class ProfileDropdown : DropdownConfigurationBaseClass {
    array<string> ids;
    string role, mod_id, setting_id, saved_label;
    int selected;
    ProfileDropdown(const string &in use, array<string> labels, array<string> values, int current,
                    const string &in mod = "", const string &in setting = "", const string &in current_label = "") {
        role = use; ids = values; selected = current; mod_id = mod; setting_id = setting; saved_label = current_label;
        @display_values = @labels;
    }
    void SetCurrentValue(IMText@ text) {
        current_option_index = max(0, min(int(display_values.length()) - 1, selected));
        text.setText(selected < 0 && saved_label != "" ? saved_label : display_values[current_option_index]);
    }
    void SetConfigValue(string value) {
        int i = display_values.find(value);
        if(i < 0) return;
        selected = i;
        if(role == "profile") { settings_profile = ids[i]; settings_skin_page = 0; profile_notice = ""; }
        else if(role == "category") { settings_profile_category = ids[i]; settings_skin_page = 0; }
        else ProfileSetModSetting(mod_id, setting_id, parseInt(ids[i]));
    }
}

FontSetup ProfileFont(const string &in label, int size, float width, vec4 color) {
    FontSetup font(button_font_small.fontName, size, color, true);
    TextMetrics bounds = GetTextAtlasMetrics("Data/Fonts/" + font.fontName + ".ttf", size, 1, label);
    if(bounds.bounds_x > width) font.size = max(15, int(size * width / bounds.bounds_x));
    return font;
}

void ProfileAction(IMDivider@ row, const string &in label, IMMessage@ message, float width,
                   bool selected = false, bool enabled = true) {
    IMContainer cell("profile_action_" + label, width, 60);
    IMImage face(button_background_diamond_thin);
    face.setSize(vec2(width - 4, 54));
    face.setColor(selected ? vec4(0.67f, 0.56f, 0.32f, 1) : vec4(0.48f, 0.48f, 0.48f, 1));
    cell.addFloatingElement(face, "background", vec2(1,3), 1);
    IMText text(label, ProfileFont(label, button_font_small.size, width-18, vec4(1)));
    text.setZOrdering(4);
    if(!enabled) text.setColor(vec4(0.53f, 0.53f, 0.53f, 1));
    cell.setElement(text);
    if(enabled) {
        cell.sendMouseOverToChildren(true);
        text.addMouseOverBehavior(text_color_mouse_over, "");
        cell.addLeftMouseClickBehavior(IMFixedMessageOnClick(message), "");
        cell.addMouseOverBehavior(button_hover_sound, "");
        AddControllerItem(cell, message);
    }
    row.append(cell);
}

void ProfileCheckbox(IMDivider@ parent, const string &in id, const string &in label, bool checked, IMMessage@ change) {
    IMContainer cell("profile_checkbox_" + id, 850, 64);
    cell.setAlignment(CALeft, CACenter);
    IMDivider row(DOHorizontal);
    IMContainer box(52, 52);
    IMImage frame(checkbox);
    frame.setSize(vec2(42));
    box.addFloatingElement(frame, "box", vec2(4), 2);
    if(checked) {
        IMImage mark(checkmark);
        mark.setSize(vec2(46));
        box.addFloatingElement(mark, "check", vec2(2), 3);
    }
    row.append(box); row.appendSpacer(16);
    IMText text(label, ProfileFont(label, button_font_small.size, 765, vec4(1)));
    text.setZOrdering(4); row.append(text);
    cell.setElement(row);
    cell.addLeftMouseClickBehavior(IMFixedMessageOnClick(change), "");
    cell.addMouseOverBehavior(button_hover_sound, "");
    AddControllerItem(cell, change);
    parent.append(cell);
}

void ProfileChooser(const string &in title, const string &in use, const array<string> &in labels,
                    const array<string> &in ids, int selected) {
    if(ids.length() == 0) return;
    const int per_page = 7;
    int start = (selected / per_page) * per_page;
    array<string> visible_labels, visible_ids;
    for(int i = start; i < min(start + per_page, int(ids.length())); ++i) {
        visible_labels.insertLast(labels[i]); visible_ids.insertLast(ids[i]);
    }
    AddDropDown(title, settings_content, DropdownConfiguration(ProfileDropdown(use, visible_labels, visible_ids, selected - start)));
    if(ids.length() <= per_page) return;
    IMDivider pages(DOHorizontal);
    IMMessage previous("profile_choose_page", use), next("profile_choose_page", use);
    previous.addString(ids[max(0,start-per_page)]);
    next.addString(ids[min(int(ids.length())-1, start+per_page)]);
    ProfileAction(pages, "Previous", previous, 200, false, start != 0);
    ProfileAction(pages, "More", next,
                  280, false, start+per_page < int(ids.length()));
    settings_content.append(pages);
}

void CosmeticCard(IMDivider@ row, JSONValue &in option, const string &in category, bool owned, bool selected) {
    const float w = 254, h = 218;
    string id = option["id"].asString();
    IMContainer card("cosmetic_" + category + "_" + id, w, h);
    vec4 rarity = ProfileRarityColor(option["rarity"].asInt());
    IMImage border(white_background);
    border.setSize(vec2(w-8,h-8)); border.setColor(rarity);
    card.addFloatingElement(border, "rarity", vec2(4), 1);
    IMImage bg(white_background);
    float inset = selected ? 9.0f : 6.0f;
    bg.setSize(vec2(w-2*inset,h-2*inset)); bg.setColor(vec4(0.07f, 0.07f, 0.07f, 1));
    card.addFloatingElement(bg, "face", vec2(inset), 2);
    string image = option["image"].asString();
    if(image != "") {
        IMImage art(image.findFirst("Data/") == 0 ? image.substr(5) : image);
        art.scaleToSizeY(132);
        if(art.getSizeX() > w-30) art.scaleToSizeX(w-30);
        if(!owned) art.setColor(vec4(0.42f,0.42f,0.42f,1));
        card.addFloatingElement(art, "art", vec2((w-art.getSizeX())/2, 13+(132-art.getSizeY())/2), 3);
    }
    string label = option["label"].asString();
    IMText text(label, ProfileFont(label, 31, w-24, vec4(1)));
    card.addFloatingElement(text, "label", vec2(UNDEFINEDSIZE,146), 4);
    if(selected || !owned) {
        IMText state(selected ? "Selected" : "Locked", FontSetup(button_font_small.fontName, 23, rarity, true));
        card.addFloatingElement(state, "state", vec2(UNDEFINEDSIZE,181), 4);
    }
    if(owned) {
        IMMessage change("profile_choose_skin"); change.addString(category); change.addString(id);
        card.addLeftMouseClickBehavior(IMFixedMessageOnClick(change), "");
        card.addMouseOverBehavior(button_hover_sound, "");
        AddControllerItem(card, change);
    }
    row.append(card);
}

void DrawProfileNameEditor() {
    settings_content.append(IMText(profile_editor_mode == "create" ? "New profile" : "Rename profile", button_font));
    AddTextInput(settings_content, profile_name_box, "profile_activate_name");
    if(!MenuOwnsKeyboard() || controller_active) {
        const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for(uint i = 0; i < chars.length();) {
            IMDivider keys(DOHorizontal);
            for(int col = 0; col < 8 && i < chars.length(); ++col, ++i) {
                string letter = chars.substr(i, 1);
                ProfileAction(keys, letter, IMMessage("profile_letter", letter), 85);
            }
            settings_content.append(keys);
        }
        IMDivider keys(DOHorizontal);
        ProfileAction(keys, "Space", IMMessage("profile_letter", " "), 210);
        ProfileAction(keys, "Erase", IMMessage("profile_erase"), 210);
        settings_content.append(keys);
    }
    IMDivider actions(DOHorizontal);
    ProfileAction(actions, "Save", IMMessage("profile_save_name"), 240);
    ProfileAction(actions, "Cancel", IMMessage("profile_cancel"), 240);
    settings_content.append(actions);
}

void DrawProfileRolls(JSONValue &in view) {
    bool cosmetics = view["meta"].asBool(), upgrades = view["upgrade_progression"].asBool();
    if(!cosmetics && !upgrades) return;
    IMDivider controls(DOHorizontal);
    int tokens = view["tokens"].asInt(), cost = view["roll_cost"].asInt();
    IMContainer wallet("profile_tokens", 170, 60);
    wallet.setElement(IMText("Tokens  " + tokens, button_font_small));
    controls.append(wallet);
    if(cosmetics) {
        bool available = view["can_roll"].asBool();
        string text = tokens >= cost && !available ? "Cosmetics complete" : "Roll cosmetic (" + cost + ")";
        ProfileAction(controls, text, IMMessage("profile_roll", "roll"), 295, false, available);
    }
    if(upgrades) {
        bool available = view["can_roll_upgrade"].asBool();
        string text = tokens >= cost && !available ? "Upgrades complete" : "Roll upgrade (" + cost + ")";
        ProfileAction(controls, text, IMMessage("profile_roll", "roll_upgrade"), 295, false, available);
    }
    settings_content.append(controls);

    JSONValue prize = view["win"];
    string kind = prize["kind"].asString();
    if((kind != "upgrade" || !upgrades) && (kind != "cosmetic" || !cosmetics)) return;
    IMContainer card("profile_latest_unlock", 760, 65);
    vec4 rarity = ProfileRarityColor(prize["rarity"].asInt());
    IMImage border(white_background); border.setSize(vec2(752,57)); border.setColor(rarity);
    card.addFloatingElement(border, "border", vec2(4), 1);
    IMImage face(white_background); face.setSize(vec2(746,51)); face.setColor(vec4(.07f,.07f,.07f,1));
    card.addFloatingElement(face, "face", vec2(7), 2);
    string path = prize["image"].asString();
    if(path != "") {
        IMImage art(path.findFirst("Data/") == 0 ? path.substr(5) : path);
        art.scaleToSizeY(45);
        if(art.getSizeX() > 48) art.scaleToSizeX(48);
        card.addFloatingElement(art, "art", vec2(14, (65-art.getSizeY())/2), 3);
    }
    string label = "Unlocked  " + prize["label"].asString();
    IMText title(label, ProfileFont(label, 30, 665, rarity));
    card.addFloatingElement(title, "prize", vec2(path == "" ? 18 : 68, UNDEFINEDSIZE), 4);
    settings_content.append(card);
}

void AddProfilesScreen() {
    JSONValue catalog = ReadProfileJSON(ProfileSettingsView());
    profile_settings_revision = ProfileRevision();
    if(profile_editor_mode == "create" || profile_editor_mode == "rename") DrawProfileNameEditor();
    else if(profile_editor_mode == "delete") {
        AddNote("Remove " + settings_profile + " and its saved mod progress?", settings_content);
        IMDivider actions(DOHorizontal);
        ProfileAction(actions, "Remove profile", IMMessage("profile_confirm_delete"), 340);
        ProfileAction(actions, "Cancel", IMMessage("profile_cancel"), 240);
        settings_content.append(actions);
    } else {
        array<string> names;
        for(int i = 0; i < int(catalog["names"].size()); ++i) names.insertLast(catalog["names"][i].asString());
        int name_index = max(0, names.find(settings_profile));
        settings_profile = names.length() > 0 ? names[name_index] : "";
        ProfileChooser("Profiles", "profile", names, names, name_index);
        IMDivider actions(DOHorizontal);
        ProfileAction(actions, "New profile", IMMessage("profile_editor", "create"), 260);
        if(settings_profile != "") {
            ProfileAction(actions, "Rename", IMMessage("profile_editor", "rename"), 210);
            ProfileAction(actions, "Remove", IMMessage("profile_editor", "delete"), 210);
        }
        settings_content.append(actions);
        if(settings_profile != "") {
            bool primary = catalog["primary"].asString() == settings_profile;
            ProfileCheckbox(settings_content, "primary", "Use for solo Play", primary,
                            IMMessage("profile_primary", primary ? "" : settings_profile));
            JSONValue view = ReadProfileJSON(ProfileView(settings_profile)), cats = catalog["categories"];
            DrawProfileRolls(view);
            array<string> ids, labels;
            for(int i = 0; i < int(cats.size()); ++i) {
                ids.insertLast(cats[i]["id"].asString()); labels.insertLast(cats[i]["label"].asString());
            }
            if(ids.length() > 0) {
                int cat = max(0, ids.find(settings_profile_category));
                settings_profile_category = ids[cat];
                settings_content.appendSpacer(24);
                ProfileChooser("Categories", "category", labels, ids, cat);
                JSONValue options = cats[cat]["options"];
                int count = options.size();
                settings_skin_page = max(0, min((count-1)/6, settings_skin_page));
                for(int i = settings_skin_page*6; i < min(count, (settings_skin_page+1)*6);) {
                    IMDivider cards(DOHorizontal);
                    for(int col=0; col<3 && i<min(count, (settings_skin_page+1)*6); ++col, ++i) {
                        string id = options[i]["id"].asString();
                        int selected = ProfileOptionIndex(options, view["selected"][ids[cat]].asString());
                        CosmeticCard(cards, options[i], ids[cat], ProfileOptionUnlocked(view, ids[cat], id, i), i == selected);
                    }
                    settings_content.append(cards);
                }
                if(count > 6) {
                    IMDivider pages(DOHorizontal);
                    ProfileAction(pages, "Previous cards", IMMessage("profile_card_page", -1), 270, false, settings_skin_page > 0);
                    ProfileAction(pages, "More cards", IMMessage("profile_card_page", 1), 270, false, (settings_skin_page+1)*6 < count);
                    settings_content.append(pages);
                }
            }
        }
    }
    if(profile_notice != "") AddNote(profile_notice, settings_content);
}

void AddModSettingsScreen() {
    profile_settings_revision = ProfileRevision();
    JSONValue mods = ReadProfileJSON(ProfileSettingsView())["mods"];
    int count = mods.size();
    if(count == 0) { AddNote("No enabled mods provide settings.", settings_content); return; }
    settings_mod_tab = max(0, min(count-1, settings_mod_tab));
    int start = settings_mod_tab/3*3;
    IMDivider tabs(DOHorizontal);
    if(count > 3) ProfileAction(tabs, "<", IMMessage("profile_mod_tab", max(0, start-3)), 70, false, start > 0);
    for(int i=start; i<min(start+3,count); ++i)
        ProfileAction(tabs, mods[i]["label"].asString(), IMMessage("profile_mod_tab", i), count>3 ? 205 : 265, settings_mod_tab==i);
    if(count > 3) ProfileAction(tabs, ">", IMMessage("profile_mod_tab", min(count-1, start+3)), 70, false, start+3 < count);
    settings_content.append(tabs); settings_content.appendSpacer(20);
    string mod = mods[settings_mod_tab]["id"].asString();
    JSONValue fields = mods[settings_mod_tab]["settings"];
    settings_mod_page = max(0, min((int(fields.size())-1)/6, settings_mod_page));
    for(int i = settings_mod_page*6; i < min(int(fields.size()), (settings_mod_page+1)*6); ++i) {
        JSONValue field = fields[i];
        if(field["type"].asString()=="checkbox") {
            bool checked = field["value"].asInt() != 0;
            IMMessage change("profile_toggle_setting", mod);
            change.addString(field["id"].asString()); change.addInt(checked ? 0 : 1);
            ProfileCheckbox(settings_content, field["id"].asString(), field["label"].asString(), checked, change);
        }
        else {
            JSONValue choices = field["options"];
            int choice_count = choices.size(), current = field["value"].asInt();
            if(choice_count == 0) continue;
            string id = field["id"].asString(), path = mod + "/" + id;
            int page = settings_choices_field == path ? settings_choices_page : current / 7;
            int offset = max(0, min((choice_count - 1) / 7, page)) * 7;
            array<string> values, labels;
            for(int n=offset; n<min(offset+7,choice_count); ++n) {
                labels.insertLast(choices[n].asString()); values.insertLast(""+n);
            }
            int selected = values.find("" + current);
            AddDropDown(field["label"].asString(), settings_content, DropdownConfiguration(
                ProfileDropdown("mod", labels, values, selected, mod, id, choices[current].asString())));
            if(choice_count > 7) {
                IMDivider pages(DOHorizontal);
                IMMessage prev("profile_choice_page", path), next("profile_choice_page", path);
                prev.addInt(page-1); next.addInt(page+1);
                ProfileAction(pages, "Previous choices", prev, 275, false, offset > 0);
                ProfileAction(pages, "More choices", next, 275, false, offset+7 < choice_count);
                settings_content.append(pages);
            }
        }
    }
    if(fields.size()>6) {
        IMDivider pages(DOHorizontal);
        ProfileAction(pages,"Previous settings",IMMessage("profile_mod_page",-1),280,false,settings_mod_page>0);
        ProfileAction(pages,"More settings",IMMessage("profile_mod_page",1),280,false,(settings_mod_page+1)*6<int(fields.size()));
        settings_content.append(pages);
    }
}

void ProcessProfileSettings(IMMessage@ msg) {
    string action = msg.name;
    if(action == "profile_editor") {
        profile_editor_mode = msg.getString(0);
        profile_name_box.ClearText();
        profile_name_box.default_text = "Profile name";
        profile_name_box.input_text = profile_editor_mode == "rename" ? settings_profile : "";
        profile_name_box.cursor_offset = 0;
        profile_name_box.max_length = 24;
        @profile_name_box.validator = ProfileNameValidator();
        profile_name_box.on_enter_message = "profile_save_name";
        profile_notice = "";
    } else if(action == "profile_activate_name") {
        if(MenuOwnsKeyboard()) { profile_name_box.Activate(); capture_input = true; }
        return;
    } else if(action == "profile_letter" || action == "profile_erase") {
        profile_name_box.Deactivate();
        profile_name_box.cursor_offset = 0;
        if(action == "profile_erase") profile_name_box.InputBackspace();
        else if(profile_name_box.input_text.length()<24) profile_name_box.input_text += msg.getString(0);
        profile_name_box.UpdateText();
        return;
    } else if(action == "profile_save_name") {
        capture_input = false;
        string name = profile_name_box.GetText();
        if(!ProfileNameValid(name)) { profile_notice = profile_name_hint; SwitchSettingsScreen(current_screen); return; }
        bool ok = profile_editor_mode == "create" ? ProfileEdit("create", name, "", "") : ProfileEdit("rename", settings_profile, "", name);
        if(ok) { settings_profile = name; profile_editor_mode = ""; profile_notice = ""; }
        else profile_notice = "Choose an unused profile name.";
        profile_name_box.Deactivate();
    } else if(action == "profile_cancel") {
        profile_editor_mode = ""; profile_name_box.Deactivate(); capture_input = false; profile_notice = "";
    } else if(action == "profile_confirm_delete") {
        if(ProfileEdit("delete", settings_profile, "", "")) settings_profile = "";
        else profile_notice = "Could not remove this profile.";
        profile_editor_mode = "";
    } else if(action == "profile_choose_page") {
        if(msg.getString(0) == "profile") settings_profile = msg.getString(1);
        else settings_profile_category = msg.getString(1);
        settings_skin_page = 0;
    } else if(action == "profile_choose_skin") {
        if(!ProfileEdit("set", settings_profile, msg.getString(0), msg.getString(1))) profile_notice = "This cosmetic is unavailable.";
    } else if(action == "profile_roll") {
        if(!ProfileEdit(msg.getString(0), settings_profile, "", "")) profile_notice = "This roll is unavailable.";
        else profile_notice = "";
    } else if(action == "profile_primary") {
        if(!ProfileEdit("primary", msg.getString(0), "", "")) profile_notice = "Could not save the solo profile.";
    } else if(action == "profile_card_page") settings_skin_page += msg.getInt(0);
    else if(action == "profile_mod_tab") { settings_mod_tab = msg.getInt(0); settings_mod_page = 0; }
    else if(action == "profile_mod_page") settings_mod_page += msg.getInt(0);
    else if(action == "profile_choice_page") { settings_choices_field = msg.getString(0); settings_choices_page = msg.getInt(0); }
    else if(action == "profile_toggle_setting") ProfileSetModSetting(msg.getString(0), msg.getString(1), msg.getInt(0));
    SwitchSettingsScreen(current_screen);
    if(action == "profile_editor" && profile_editor_mode != "delete" && MenuOwnsKeyboard() && !controller_active) {
        profile_name_box.Activate();
        capture_input = true;
    }
}
