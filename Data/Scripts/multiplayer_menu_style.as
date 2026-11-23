// Multiplayer uses the same artwork, typefaces and hover feedback as the
// game's menus. Native widget IDs retain keyboard/controller navigation and
// scrolling while their presentation uses the game's menu assets.

const uint kMPTextureFlags = TextureLoadFlags_NoMipmap | TextureLoadFlags_NoReduce | TextureLoadFlags_NoConvert;
TextureAssetRef mp_diamond;
TextureAssetRef mp_diamond_light;
TextureAssetRef mp_brush;
TextureAssetRef mp_sidebar;
TextureAssetRef mp_checkbox;
TextureAssetRef mp_checkmark;
float mp_window_scale = 1.0f;
bool mp_style_loaded = false;
uint mp_last_hover = 0;
uint mp_frame_hover = 0;
uint mp_frame_focus = 0;

void MPStyleLoad() {
    if(mp_style_loaded) { return; }
    mp_diamond = LoadTexture("Data/" + button_background_diamond, kMPTextureFlags);
    mp_diamond_light = LoadTexture("Data/" + button_background_diamond_light, kMPTextureFlags);
    mp_brush = LoadTexture("Data/" + brushstroke_background, kMPTextureFlags);
    mp_sidebar = LoadTexture("Data/" + black_background, kMPTextureFlags);
    mp_checkbox = LoadTexture("Data/" + checkbox, kMPTextureFlags);
    mp_checkmark = LoadTexture("Data/" + checkmark, kMPTextureFlags);
    mp_style_loaded = true;
}

void MPStyleBegin() {
    MPStyleLoad();
    mp_frame_hover = 0;
    mp_frame_focus = 0;
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
    // Mouse hover and keyboard focus may belong to different buttons. Select
    // one feedback target after the whole frame, instead of letting those two
    // widgets repeatedly announce each other on every draw.
    uint target = mp_frame_hover != 0 ? mp_frame_hover : mp_frame_focus;
    if(target != 0 && target != mp_last_hover) PlaySoundGroup("Data/Sounds/footstep_mud.xml");
    mp_last_hover = target;
    ImGui_PopStyleVar(5);
    ImGui_PopStyleColor(21);
    ImGui_PopFont();
}

void MPWindowScale(float scale) {
    mp_window_scale = scale;
    ImGui_SetWindowFontScale(scale);
}

void MPPanelBackground() {
    vec2 position = ImGui_GetWindowPos();
    ImDrawList_AddImage(mp_sidebar, position, position + ImGui_GetWindowSize(),
                       vec2(0), vec2(1), ImGui_GetColorU32(vec4(1, 1, 1, 0.84f)));
}

void MPHeading(const string &in text, float height = 38.0f) {
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
    bool hovered = ImGui_IsItemHovered() || ImGui_IsItemFocused();
    if(ImGui_IsItemHovered()) mp_frame_hover = ImGui_GetID(label);
    if(ImGui_IsItemFocused()) mp_frame_focus = ImGui_GetID(label);
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
        ImGui_SetWindowFontScale(mp_window_scale * fit);
        text_size = ImGui_CalcTextSize(text);
    }
    ImDrawList_AddText(a + move + vec2((b.x - a.x - text_size.x) * 0.5f,
                       (b.y - a.y - text_size.y) * 0.5f),
                       ImGui_GetColorU32(hovered ? vec4(1) : vec4(0.8f, 0.8f, 0.8f, 1)), text);
    ImGui_SetWindowFontScale(mp_window_scale);
    ImGui_PopFont();
    return pressed;
}

bool MPCheckbox(const string &in label, bool &inout checked) {
    ImGui_PushMenuFont(true);
    float fit = min(1.0f, (ImGui_GetContentRegionAvailWidth() - 38.0f) /
                    max(1.0f, ImGui_CalcTextSize(label).x));
    ImGui_SetWindowFontScale(mp_window_scale * max(0.5f, fit));
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
    ImGui_SetWindowFontScale(mp_window_scale);
    ImGui_PopFont();
    if(changed) { PlaySoundGroup("Data/Sounds/hit/hit_hard.xml"); }
    return changed;
}

bool MPTeamCombo(int &inout current, int count) {
    vec2 position = ImGui_GetCursorScreenPos();
    float width = ImGui_CalcItemWidth();
    bool changed = false;
    bool open = ImGui_BeginCombo("##team", "      " + (current + 1));
    if(open) {
        for(int team = 0; team < count; ++team) {
            vec2 option = ImGui_GetCursorScreenPos();
            bool selected = team == current;
            if(ImGui_Selectable("      " + (team + 1), selected)) {
                current = team;
                changed = true;
            }
            if(selected) { ImGui_SetItemDefaultFocus(); }
            vec3 color = LobbyTeamColor(team + 1);
            ImDrawList_AddRectFilled(option + vec2(4, 4), option + vec2(17, 17),
                                    ImGui_GetColorU32(vec4(color.x, color.y, color.z, 1)));
        }
        ImGui_EndCombo();
    }
    // BeginCombo switches the current draw list while its popup is open; draw
    // the preview after EndCombo so the swatch stays in the player's row.
    vec3 color = LobbyTeamColor(current + 1);
    ImDrawList_AddRectFilled(position + vec2(6, 7), position + vec2(19, 20),
                            ImGui_GetColorU32(vec4(color.x, color.y, color.z, 1)));
    return changed;
}
