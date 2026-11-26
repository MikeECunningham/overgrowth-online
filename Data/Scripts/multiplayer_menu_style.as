// Multiplayer uses the same artwork, typefaces and hover feedback as the
// game's menus. Native widget IDs retain keyboard/controller navigation and
// scrolling while their presentation uses the game's menu assets.

const uint kMPTextureFlags = TextureLoadFlags_NoMipmap | TextureLoadFlags_NoReduce | TextureLoadFlags_NoConvert;
TextureAssetRef mp_diamond;
TextureAssetRef mp_diamond_light;
TextureAssetRef mp_dropdown;
TextureAssetRef mp_arrow;
TextureAssetRef mp_brush;
TextureAssetRef mp_sidebar;
TextureAssetRef mp_checkbox;
TextureAssetRef mp_checkmark;
float mp_window_scale = 1.0f;
const float kMPTextScale = 0.9f;
bool mp_style_loaded = false;
uint mp_last_hover = 0;
uint mp_frame_hover = 0;
bool mp_pointer_active = true;

void MPStyleLoad() {
    if(mp_style_loaded) { return; }
    mp_diamond = LoadTexture("Data/" + button_background_diamond, kMPTextureFlags);
    mp_diamond_light = LoadTexture("Data/" + button_background_diamond_light, kMPTextureFlags);
    mp_dropdown = LoadTexture("Data/" + button_background_diamond_thin, kMPTextureFlags);
    mp_arrow = LoadTexture("Data/" + arrow_icon, kMPTextureFlags);
    mp_brush = LoadTexture("Data/" + brushstroke_background, kMPTextureFlags);
    mp_sidebar = LoadTexture("Data/" + black_background, kMPTextureFlags);
    mp_checkbox = LoadTexture("Data/" + checkbox, kMPTextureFlags);
    mp_checkmark = LoadTexture("Data/" + checkmark, kMPTextureFlags);
    mp_style_loaded = true;
}

void MPStyleBegin() {
    MPStyleLoad();
    mp_frame_hover = 0;
    mp_pointer_active = GetMenuInputSource(controller_player) == MenuMouse;
    ImGui_PushMenuFont(false);
    ImGui_PushStyleColor(ImGuiCol_Text, vec4(0.82f, 0.82f, 0.82f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_TextDisabled, vec4(0.39f, 0.39f, 0.39f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_WindowBg, vec4(0, 0, 0, 0));
    ImGui_PushStyleColor(ImGuiCol_ChildBg, vec4(0, 0, 0, 0.20f));
    ImGui_PushStyleColor(ImGuiCol_PopupBg, vec4(0.025f, 0.025f, 0.025f, 0.98f));
    ImGui_PushStyleColor(ImGuiCol_Border, vec4(0.3f, 0.3f, 0.3f, 0.6f));
    ImGui_PushStyleColor(ImGuiCol_FrameBg, vec4(0.04f, 0.04f, 0.04f, 0.95f));
    ImGui_PushStyleColor(ImGuiCol_FrameBgHovered, vec4(0.16f, 0.16f, 0.16f, 0.95f));
    ImGui_PushStyleColor(ImGuiCol_FrameBgActive, vec4(0.20f, 0.20f, 0.20f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_Button, vec4(0.08f, 0.08f, 0.08f, 0.9f));
    ImGui_PushStyleColor(ImGuiCol_ButtonHovered, vec4(0.21f, 0.21f, 0.21f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ButtonActive, vec4(0.29f, 0.29f, 0.29f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_Header, vec4(0.32f, 0.29f, 0.15f, 0.5f));
    ImGui_PushStyleColor(ImGuiCol_HeaderHovered, vec4(0.28f, 0.28f, 0.28f, 0.9f));
    ImGui_PushStyleColor(ImGuiCol_HeaderActive, vec4(0.40f, 0.37f, 0.22f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_CheckMark, vec4(0.88f, 0.84f, 0.62f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ScrollbarBg, vec4(0, 0, 0, 0.15f));
    ImGui_PushStyleColor(ImGuiCol_ScrollbarGrab, vec4(0.36f, 0.36f, 0.36f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ScrollbarGrabHovered, vec4(0.62f, 0.59f, 0.39f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ScrollbarGrabActive, vec4(0.85f, 0.80f, 0.53f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_NavHighlight, vec4(0.93f, 0.86f, 0.52f, 1.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui_PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui_PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui_PushStyleVar(ImGuiStyleVar_FramePadding, vec2(7.0f, 5.0f));
    ImGui_PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, vec2(8.0f, 6.0f));
}

void MPStyleEnd() {
    if(mp_frame_hover != 0 && mp_frame_hover != mp_last_hover) PlaySoundGroup("Data/Sounds/footstep_mud.xml");
    mp_last_hover = mp_frame_hover;
    ImGui_PopStyleVar(5);
    ImGui_PopStyleColor(21);
    ImGui_PopFont();
}

void MPWindowScale(float scale) {
    mp_window_scale = scale;
    ImGui_SetWindowFontScale(scale * kMPTextScale);
}

void MPPanelBackground() {
    vec2 position = ImGui_GetWindowPos();
    ImDrawList_AddImage(mp_sidebar, position, position + ImGui_GetWindowSize(),
                       vec2(0), vec2(1), ImGui_GetColorU32(vec4(1, 1, 1, 0.84f)));
}

void MPHeading(const string &in text, float height = 44.0f) {
    vec2 position = ImGui_GetCursorScreenPos();
    float width = ImGui_GetContentRegionAvailWidth();
    ImGui_Dummy(vec2(width, height * mp_window_scale));
    ImDrawList_AddImage(mp_brush, position, position + vec2(width, height * mp_window_scale),
                       vec2(0), vec2(1), ImGui_GetColorU32(vec4(1)));
    ImGui_PushMenuFont(true);
    vec2 text_size = ImGui_CalcTextSize(text);
    ImDrawList_AddText(position + vec2((width - text_size.x) * 0.5f,
                       (height * mp_window_scale - text_size.y) * 0.5f),
                       ImGui_GetColorU32(vec4(0.85f, 0.85f, 0.85f, 1)), text);
    ImGui_PopFont();
}

string MPWrapLabel(const string &in label, float width) {
    string text = label.split("##")[0];
    array<string>@ words = text.split(" ");
    string result;
    string line;
    for(uint i = 0; i < words.length(); ++i) {
        string next = line == "" ? words[i] : line + " " + words[i];
        if(line != "" && ImGui_CalcTextSize(next).x > width) {
            result += line + "\n";
            line = words[i];
        } else {
            line = next;
        }
    }
    return result + line;
}

// A normal button supplies hit testing and controller activation. Artwork and
// the same movement/color/sound feedback as AddButton supply its appearance.
bool MPButton(const string &in label, const vec2 &in size = vec2(0), bool prominent = false) {
    ImGui_PushStyleColor(ImGuiCol_Text, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_Button, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_ButtonHovered, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_ButtonActive, vec4(0));
    bool pressed = ImGui_Button(label, size);
    ImGui_PopStyleColor(4);
    vec2 a = ImGui_GetItemRectMin();
    vec2 b = ImGui_GetItemRectMax();
    bool hovered = ImGui_IsItemHovered();
    if(hovered) mp_frame_hover = ImGui_GetID(label);
    if(pressed) { PlaySoundGroup("Data/Sounds/hit/hit_hard.xml"); }
    float offset = hovered ? min(4.0f, (b.x - a.x) * 0.01f) : 0.0f;
    vec2 move(offset, 0);
    ImDrawList_AddImage(mp_diamond, a + move, b + move, vec2(0), vec2(1),
                       ImGui_GetColorU32(vec4(1)));
    if(hovered || prominent) {
        ImDrawList_AddImage(mp_diamond_light, a + move, b + move, vec2(0), vec2(1),
                           ImGui_GetColorU32(vec4(1, 1, 1, hovered ? 0.16f : 0.06f)));
    }
    ImGui_PushMenuFont(true);
    string text = MPWrapLabel(label, max(30.0f, b.x - a.x - 24.0f));
    vec2 text_size = ImGui_CalcTextSize(text);
    float fit = min(1.0f, (b.y - a.y - 8.0f) / max(1.0f, text_size.y));
    if(fit < 1.0f) {
        ImGui_SetWindowFontScale(mp_window_scale * kMPTextScale * fit);
        text_size = ImGui_CalcTextSize(text);
    }
    ImDrawList_AddText(a + move + vec2((b.x - a.x - text_size.x) * 0.5f,
                       (b.y - a.y - text_size.y) * 0.5f),
                       ImGui_GetColorU32(hovered ? vec4(1) : vec4(0.8f, 0.8f, 0.8f, 1)), text);
    ImGui_SetWindowFontScale(mp_window_scale * kMPTextScale);
    ImGui_PopFont();
    return pressed;
}

bool MPCheckbox(const string &in label, bool &inout checked) {
    ImGui_PushMenuFont(true);
    float fit = min(1.0f, (ImGui_GetContentRegionAvailWidth() - 38.0f) /
                    max(1.0f, ImGui_CalcTextSize(label).x));
    ImGui_SetWindowFontScale(mp_window_scale * kMPTextScale * max(0.5f, fit));
    ImGui_PushStyleColor(ImGuiCol_FrameBg, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_FrameBgHovered, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_FrameBgActive, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_CheckMark, vec4(0));
    bool changed = ImGui_Checkbox(label, checked);
    ImGui_PopStyleColor(4);
    vec2 a = ImGui_GetItemRectMin();
    float side = ImGui_GetTextLineHeight() + 10.0f;
    ImDrawList_AddImage(mp_checkbox, a, a + vec2(side), vec2(0), vec2(1), ImGui_GetColorU32(vec4(1)));
    if(checked) {
        ImDrawList_AddImage(mp_checkmark, a, a + vec2(side), vec2(0), vec2(1), ImGui_GetColorU32(vec4(1)));
    }
    ImGui_SetWindowFontScale(mp_window_scale * kMPTextScale);
    ImGui_PopFont();
    if(changed) { PlaySoundGroup("Data/Sounds/hit/hit_hard.xml"); }
    return changed;
}

// Keep native popup/navigation behavior and draw the same thin diamond,
// arrow, typeface and black option rows used by Settings dropdowns.
bool MPBeginCombo(const string &in label, const string &in preview, float left_padding = 18.0f) {
    ImGui_PushMenuFont(true);
    vec2 a = ImGui_GetCursorScreenPos();
    vec2 b = a + vec2(ImGui_CalcItemWidth(), ImGui_GetFrameHeight());
    bool hovered = mp_pointer_active && ImGui_IsWindowHovered() && ImGui_IsMouseHoveringRect(a, b, true);
    ImDrawList_AddImage(mp_dropdown, a, b, vec2(0), vec2(1), ImGui_GetColorU32(vec4(1)));
    if(hovered) ImDrawList_AddImage(mp_diamond_light, a, b, vec2(0), vec2(1), ImGui_GetColorU32(vec4(1, 1, 1, 0.12f)));
    vec2 text_size = ImGui_CalcTextSize(preview);
    ImDrawList_AddText(a + vec2(left_padding, (b.y - a.y - text_size.y) * 0.5f),
        ImGui_GetColorU32(hovered ? vec4(1) : vec4(0.8f, 0.8f, 0.8f, 1)), preview);
    float side = min(22.0f * mp_window_scale, (b.y - a.y) * 0.65f);
    vec2 arrow_at(b.x - side - 12.0f, a.y + (b.y - a.y - side) * 0.5f);
    ImDrawList_AddImageQuad(mp_arrow, arrow_at, arrow_at + vec2(side, 0), arrow_at + vec2(side), arrow_at + vec2(0, side),
        vec2(1, 0), vec2(1, 1), vec2(0, 1), vec2(0, 0), ImGui_GetColorU32(vec4(0.8f, 0.8f, 0.8f, 1)));
    ImGui_PushStyleColor(ImGuiCol_Text, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_FrameBg, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_FrameBgHovered, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_FrameBgActive, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_Button, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_ButtonHovered, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_ButtonActive, vec4(0));
    bool open = ImGui_BeginCombo(label, preview, ImGuiComboFlags_HeightLarge);
    ImGui_PopStyleColor(7);
    if(open) {
        MPWindowScale(mp_window_scale);
        MPPanelBackground();
    } else ImGui_PopFont();
    return open;
}

void MPEndCombo() {
    ImGui_EndCombo();
    ImGui_PopFont();
}

bool MPComboOption(const string &in label, bool selected, float left_padding = 14.0f) {
    ImGui_PushStyleColor(ImGuiCol_Text, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_Header, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_HeaderHovered, vec4(0));
    ImGui_PushStyleColor(ImGuiCol_HeaderActive, vec4(0));
    bool pressed = ImGui_Selectable(label, selected, 0, vec2(0, 36.0f * mp_window_scale));
    ImGui_PopStyleColor(4);
    vec2 a = ImGui_GetItemRectMin(), b = ImGui_GetItemRectMax();
    bool hovered = ImGui_IsItemHovered();
    if(hovered) mp_frame_hover = ImGui_GetID(label);
    if(hovered || selected) ImDrawList_AddImage(mp_dropdown, a, b, vec2(0), vec2(1), ImGui_GetColorU32(vec4(1)));
    vec2 size = ImGui_CalcTextSize(label.split("##")[0]);
    ImDrawList_AddText(a + vec2(left_padding, (b.y - a.y - size.y) * 0.5f),
        ImGui_GetColorU32(hovered || selected ? vec4(1) : vec4(0.8f, 0.8f, 0.8f, 1)), label.split("##")[0]);
    if(pressed) PlaySoundGroup("Data/Sounds/hit/hit_hard.xml");
    return pressed;
}

bool MPCombo(const string &in label, int &inout current, const array<string> &in options) {
    if(options.length() == 0) return false;
    current = max(0, min(int(options.length()) - 1, current));
    bool changed = false;
    if(MPBeginCombo(label, options[current])) {
        for(uint i = 0; i < options.length(); ++i) {
            ImGui_PushID(int(i));
            if(MPComboOption(options[i], int(i) == current)) { current = int(i); changed = true; }
            if(int(i) == current) ImGui_SetItemDefaultFocus();
            ImGui_PopID();
        }
        MPEndCombo();
    }
    return changed;
}

bool MPTeamCombo(int &inout current, int count) {
    vec2 position = ImGui_GetCursorScreenPos();
    bool changed = false;
    bool open = MPBeginCombo("##team", "" + (current + 1), 34.0f);
    if(open) {
        for(int team = 0; team < count; ++team) {
            vec2 option = ImGui_GetCursorScreenPos();
            bool selected = team == current;
            if(MPComboOption("" + (team + 1), selected, 34.0f)) {
                current = team;
                changed = true;
            }
            if(selected) { ImGui_SetItemDefaultFocus(); }
            vec3 color = LobbyTeamColor(team + 1);
            ImDrawList_AddRectFilled(option + vec2(8, 10), option + vec2(23, 25),
                                    ImGui_GetColorU32(vec4(color.x, color.y, color.z, 1)));
        }
        MPEndCombo();
    }
    // BeginCombo switches the current draw list while its popup is open; draw
    // the preview after EndCombo so the swatch stays in the player's row.
    vec3 color = LobbyTeamColor(current + 1);
    ImDrawList_AddRectFilled(position + vec2(10, 12), position + vec2(25, 27),
                            ImGui_GetColorU32(vec4(color.x, color.y, color.z, 1)));
    return changed;
}
