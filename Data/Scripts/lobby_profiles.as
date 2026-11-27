#include "profile_gui_common.as"

class LobbyAppearanceCategory {
    string id;
    array<string> ids, labels;
    int selected = 0;
}

class LobbyProfileRow {
    uint seat;
    JSONValue view;
    string name;
    array<string> names, category_names;
    array<LobbyAppearanceCategory@> categories;

    uint archive_revision, roster_revision, actor;
    bool loaded = false;
    int category = 0, held_shortcuts = 0;
    int win_serial = 0, error_serial = 0;
    string problem;
    float reveal_until = 0.0f;
    vec4 cosmetic_roll_rect, upgrade_roll_rect;
    string art_path;
    TextureAssetRef art;
}
array<LobbyProfileRow@> profile_rows;
int new_profile_seat = -1;
bool focus_profile_name = false;
string new_profile_name;
bool profile_keyboard = false;

LobbyProfileRow@ LobbyProfile(uint seat) {
    LobbyProfileRow@ row;
    for(uint i = 0; i < profile_rows.length(); ++i)
        if(profile_rows[i].seat == seat) { @row = profile_rows[i]; break; }
    if(row is null) {
        @row = LobbyProfileRow();
        row.seat = seat;
        profile_rows.insertLast(row);
    }
    uint archive = ProfileRevision(), roster = lobby_revision, actor = LobbyCurrentPlayerID();
    if(!row.loaded || row.archive_revision != archive || row.roster_revision != roster || row.actor != actor) {
        JSON doc;
        doc.parseString(LobbyProfileView(seat));
        row.view = doc.getRoot();
        row.name = row.view["name"].asString();
        JSONValue names = row.view["names"], cats = row.view["categories"];
        row.names.resize(0);
        for(int i = 0; i < int(names.size()); ++i) row.names.insertLast(names[i].asString());
        row.category_names.resize(0);
        row.categories.resize(0);
        for(int i = 0; i < int(cats.size()); ++i) {
            LobbyAppearanceCategory cat;
            cat.id = cats[i]["id"].asString();
            row.category_names.insertLast(cats[i]["label"].asString());
            JSONValue options = cats[i]["options"];
            for(int n = 0; n < int(options.size()); ++n) {
                string option = options[n]["id"].asString();
                if(!ProfileOptionUnlocked(row.view, cat.id, option, n)) continue;
                cat.ids.insertLast(option);
                cat.labels.insertLast(options[n]["label"].asString());
            }
            cat.selected = max(0, cat.ids.find(row.view["selected"][cat.id].asString()));
            row.categories.insertLast(cat);
        }
        int win = row.view["win"]["serial"].asInt();
        if(row.loaded && win > 0 && win != row.win_serial) row.reveal_until = ImGui_GetTime() + 1.75f;
        row.win_serial = win;
        int failure = row.view["error"]["serial"].asInt();
        if(failure != 0 && failure != row.error_serial) row.problem = row.view["error"]["message"].asString();
        row.error_serial = failure;
        row.archive_revision = archive;
        row.roster_revision = roster;
        row.actor = actor;
        row.loaded = true;
    }
    return row;
}

bool EditLobbyProfile(LobbyProfileRow@ row, const string &in action, const string &in key, const string &in value) {
    bool ok = LobbyProfileEdit(row.seat, action, key, value);
    if(ok) { row.archive_revision = ProfileRevision() - 1; row.problem = ""; }
    return ok;
}

void FocusProfileSelector() {
    if(focus_lobby_form && LobbyInitialForm() == "Players") {
        ImGui_SetItemDefaultFocus();
        ImGui_SetKeyboardFocusHere(-1);
        focus_lobby_form = false;
    }
}

void DrawProfileSelector(LobbyProfileRow@ row, float scale) {
    bool editable = row.view["can_edit"].asBool() && !LobbyMigrationBusy();
    ImGui_BeginDisabled(!editable);
    ImGui_PushItemWidth(-1);
    if(new_profile_seat == int(row.seat)) {
        bool focus_keys = focus_profile_name && profile_keyboard;
        if(focus_profile_name && !profile_keyboard) ImGui_SetKeyboardFocusHere();
        focus_profile_name = false;
        bool submit = ImGui_InputText("##new_profile_name", new_profile_name, 25, ImGuiInputTextFlags_EnterReturnsTrue);
        if(profile_keyboard) {
            const string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            float key_w = (ImGui_GetContentRegionAvailWidth() - 5.0f * 2.0f) / 6.0f;
            ImGui_PushStyleVar(ImGuiStyleVar_ItemSpacing, vec2(2, 2));
            for(uint i = 0; i < chars.length(); ++i) {
                if(i % 6 != 0) ImGui_SameLine(0, 2);
                if(ImGui_Button(chars.substr(i, 1), vec2(key_w, 24.0f * scale)) && new_profile_name.length() < 24)
                    new_profile_name += chars.substr(i, 1);
                if(i == 0 && focus_keys) { ImGui_SetItemDefaultFocus(); ImGui_SetKeyboardFocusHere(-1); }
            }
            if(ImGui_Button("Space") && new_profile_name.length() < 24) new_profile_name += " ";
            ImGui_SameLine();
            if(ImGui_Button("Erase") && new_profile_name.length() > 0) new_profile_name.resize(new_profile_name.length() - 1);
            ImGui_PopStyleVar();
        }
        if(ImGui_Button("Create") || submit) {
            if(!ProfileNameValid(new_profile_name)) row.problem = profile_name_hint;
            else if(EditLobbyProfile(row, "create", "", new_profile_name)) {
                new_profile_seat = -1;
                notice = "";
            } else row.problem = "Choose an unused profile name.";
        }
        ImGui_SameLine();
        if(ImGui_Button("Cancel")) new_profile_seat = -1;
    } else {
        string picked = row.name;
        bool choosing_new = false, change = false;
        if(MPBeginCombo("##profile", row.name == "" ? "Stay as Guest" : row.name)) {
            if(MPComboOption("Stay as Guest", row.name == "")) { picked = ""; change = true; }
            if(row.name == "") ImGui_SetItemDefaultFocus();
            if(MPComboOption("New Profile", false)) { choosing_new = true; change = true; }
            array<string> occupied;
            for(uint i = 0; i < lobby_players.length(); ++i) {
                LobbyPlayer other = lobby_players[i];
                if(other.id != row.seat && other.connected && !other.bot && !other.placeholder)
                    occupied.insertLast(LobbyProfile(other.id).name);
            }
            for(uint i = 0; i < row.names.length(); ++i) {
                const string name = row.names[i];
                bool taken = occupied.find(name) >= 0;
                ImGui_PushID(int(i));
                ImGui_BeginDisabled(taken);
                if(MPComboOption(name + (taken ? " (in use)" : ""), name == row.name)) { picked = name; change = true; }
                if(name == row.name) ImGui_SetItemDefaultFocus();
                ImGui_EndDisabled();
                ImGui_PopID();
            }
            MPEndCombo();
        }
        if(change) {
            if(choosing_new) {
                // Creation always starts from a guest. Cancelling leaves that seat temporary.
                if(EditLobbyProfile(row, "select", "", "")) {
                    new_profile_seat = int(row.seat);
                    new_profile_name = "";
                    focus_profile_name = true;
                    profile_keyboard = LobbyCurrentLocalPlayerIndex() != GetLocalKeyboardPlayerIndex();
                }
            } else if(!EditLobbyProfile(row, "select", "", picked)) {
                row.problem = "This profile is unavailable.";
            }
        }
        if(editable) FocusProfileSelector();
        LobbyHint("Choose a profile on the host. Guest appearance changes last for this lobby.");
    }
    ImGui_PopItemWidth();
    ImGui_EndDisabled();
}

void DrawProfileReveal(LobbyProfileRow@ row, vec2 a, vec2 b, float scale) {
    float left = row.reveal_until - ImGui_GetTime();
    if(left <= 0.0f) return;
    JSONValue option = row.view["win"];
    vec2 window = ImGui_GetWindowPos(), size = ImGui_GetWindowSize();
    a.y = max(a.y, window.y);
    b.y = min(b.y, window.y + size.y);
    if(b.y <= a.y) return;
    float alpha = min(1.0f, left * 5.0f);
    ImDrawList_AddRectFilled(a, b, ImGui_GetColorU32(vec4(0.02f, 0.02f, 0.02f, 0.91f * alpha)));
    float width = min(310.0f * scale, b.x-a.x-12);
    float height = min(100.0f * scale, b.y-a.y-8);
    if(width < 5 || height < 5) return;
    vec2 pos = (a+b-vec2(width,height))*0.5f;
    vec4 tint = ProfileRarityColor(option["rarity"].asInt()); tint.a = alpha;
    ImDrawList_AddRectFilled(pos, pos+vec2(width,height), ImGui_GetColorU32(tint));
    ImDrawList_AddRectFilled(pos+vec2(3), pos+vec2(width,height)-vec2(3), ImGui_GetColorU32(vec4(0.08f,0.08f,0.08f,alpha)));
    string path = option["image"].asString();
    if(row.art_path != path) {
        row.art_path = path;
        row.art.Clear();
        if(path != "") row.art = LoadTexture(path, kMPTextureFlags);
    }
    float side = row.art.IsValid() ? min(height-12, width*0.34f) : 0.0f;
    if(row.art.IsValid()) ImDrawList_AddImage(row.art, pos+vec2(6), pos+vec2(6+side), vec2(0), vec2(1), ImGui_GetColorU32(vec4(1,1,1,alpha)));
    string caption = MPWrapLabel(option["label"].asString(), width-side-24);
    vec2 text_size = ImGui_CalcTextSize(caption);
    float text_x = side > 0 ? side+13 : (width-text_size.x)*0.5f;
    ImDrawList_AddText(pos+vec2(text_x,(height-text_size.y)*0.5f), ImGui_GetColorU32(tint), caption);
}

string LobbyRollLabel(LobbyProfileRow@ row, bool upgrade) {
    const int cost = row.view["roll_cost"].asInt();
    const bool available = row.view[upgrade ? "can_roll_upgrade" : "can_roll"].asBool();
    if(!available && row.view["tokens"].asInt() >= cost) return upgrade ? "Upgrades complete" : "Cosmetics complete";
    return (upgrade ? "Roll upgrade (" : "Roll cosmetic (") + cost + ")";
}

vec4 DrawLobbyRoll(LobbyProfileRow@ row, bool upgrade, const string &in label, float width) {
    bool available = row.view[upgrade ? "can_roll_upgrade" : "can_roll"].asBool();
    bool complete = !available && row.view["tokens"].asInt() >= row.view["roll_cost"].asInt();
    ImGui_BeginDisabled(!available || row.reveal_until > ImGui_GetTime());
    if(ImGui_Button(label, vec2(width,0))) EditLobbyProfile(row, upgrade ? "roll_upgrade" : "roll", "", "");
    vec2 a = ImGui_GetItemRectMin(), b = ImGui_GetItemRectMax();
    ImGui_EndDisabled();
    LobbyHint(complete ? "Everything in this collection is unlocked." :
              upgrade ? "Add an upgrade to the shared shop on the next map start." : "Unlock an unowned cosmetic.");
    return vec4(a.x, a.y, b.x, b.y);
}

void DrawLobbyCosmetics(LobbyProfileRow@ row, float scale) {
    int count = row.categories.length();
    if(count == 0 || new_profile_seat == int(row.seat)) return;
    row.category = max(0, min(count - 1, row.category));
    bool editable = row.view["can_edit"].asBool() && !LobbyMigrationBusy();
    bool shortcuts = editable && row.seat == LobbyCurrentPlayerID() && editing_name < 0 &&
        !lobby_difficulty_open && !ImGui_IsAnyPopupOpen() && !ImGui_IsAnyItemActive() &&
        !GetInputDown(controller_player, "menu_window");
    // Drawing and simulation can tick at different rates. Latch physical
    // presses until released so each trigger/bumper advances exactly once.
    int input = LobbyGetCurrentInputID();
    int held = row.seat != LobbyCurrentPlayerID() ? 0 :
        (GetInputDown(input,"menu_profile_category_prev") ? 1 : 0) |
        (GetInputDown(input,"menu_profile_category_next") ? 2 : 0) |
        (GetInputDown(input,"menu_profile_skin_prev") ? 4 : 0) |
        (GetInputDown(input,"menu_profile_skin_next") ? 8 : 0);
    int pressed = shortcuts ? held & ~row.held_shortcuts : 0;
    row.held_shortcuts = held;
    int turn = ((pressed & 2) != 0 ? 1 : 0) - ((pressed & 1) != 0 ? 1 : 0);
    int step = ((pressed & 8) != 0 ? 1 : 0) - ((pressed & 4) != 0 ? 1 : 0);
    row.category = (row.category + turn + count) % count;
    float width = ImGui_GetContentRegionAvailWidth();
    ImGui_BeginDisabled(!editable);
    ImGui_PushItemWidth((width - 8.0f) * 0.49f);
    MPCombo("##cosmetic_category", row.category, row.category_names);
    LobbyHint("L2 / R2 changes cosmetic category.");
    ImGui_PopItemWidth();
    ImGui_SameLine(0, 8);
    LobbyAppearanceCategory@ cat = row.categories[row.category];
    if(cat.ids.length() > 0) {
        int selected = cat.selected;
        ImGui_PushItemWidth(-1);
        bool pick = MPCombo("##cosmetic_option", selected, cat.labels);
        if(step != 0) selected = (selected + step + int(cat.ids.length())) % int(cat.ids.length());
        if(pick || step != 0) EditLobbyProfile(row, "set", cat.id, cat.ids[selected]);
        LobbyHint("L1 / R1 changes the selected appearance.");
        ImGui_PopItemWidth();
    }
    bool cosmetic = row.view["meta"].asBool();
    bool upgrades = row.view["upgrade_progression"].asBool();
    if(cosmetic || upgrades) {
        string wallet = "Tokens: " + row.view["tokens"].asInt();
        string cosmetic_label = LobbyRollLabel(row, false), upgrade_label = LobbyRollLabel(row, true);
        // MPStyle sets seven pixels of horizontal frame padding. Use the actual
        // label widths so both buttons fit the scrollable roster at any UI scale.
        float cosmetic_w = min(width, ImGui_CalcTextSize(cosmetic_label).x + 16.0f);
        float upgrade_w = min(width, ImGui_CalcTextSize(upgrade_label).x + 16.0f);
        const float gap = 6.0f;
        float buttons = (cosmetic ? cosmetic_w : 0.0f) + (upgrades ? upgrade_w : 0.0f);
        if(cosmetic && upgrades) buttons += gap;
        bool inline_wallet = ImGui_CalcTextSize(wallet).x + gap + buttons <= width;
        ImGui_Text(wallet);
        if(inline_wallet) ImGui_SameLine(0, gap);
        if(cosmetic) row.cosmetic_roll_rect = DrawLobbyRoll(row, false, cosmetic_label, cosmetic_w);
        if(upgrades) {
            if(cosmetic && buttons <= width) ImGui_SameLine(0,gap);
            row.upgrade_roll_rect = DrawLobbyRoll(row, true, upgrade_label, upgrade_w);
        }
    }
    ImGui_EndDisabled();
}
