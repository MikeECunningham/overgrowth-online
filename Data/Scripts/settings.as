//-----------------------------------------------------------------------------
//           Name: settings.as
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

const int kInvalidControllerButton = -1234; // Banking on controllers never having 1234 axes
const int kInvalidKeyboardButton = -1;
const int kInvalidMouseButton = -1;
const int kMaxShownKeybinds = 10;

IMDivider@ settings_content;
string current_screen;
int active_rebind = -1;
int controller_sequence_id;
int keyboard_sequence_id;
int mouse_sequence_id;

IMText@ apply_label;
IMText@ cancel_label;
IMText@ rebind_text;
IMText@ hint_text;
string rebind_key = "";
bool allow_rebind = true; // User must release menu_return before allowing another keybind

int potential_rebind_scancode;
int potential_rebind_mouse_button;
int potential_rebind_controller_button;
ScrollDir potential_rebind_scroll_dir;
enum ScrollDir { SCROLL_NONE, SCROLL_UP, SCROLL_DOWN, SCROLL_LEFT, SCROLL_RIGHT };

int show_keybinds_offset = 0;
IMContainer@ key_rebind_container;

const float background_height = 1300;
const float background_width = 1600;
IMContainer@ main_container;

bool controller_connected = false;
uint settings_revision = 0;
bool settings_personal_only = false;
bool settings_match_host = true;

bool popup_open = false;
const string popup_name = "popup";
IMDivider@ OpenPopup(int width, int height) {
    if(popup_open) {
        main_container.removeElement(popup_name);
        main_container.setPauseBehaviors(false);
    }

    IMContainer popup_container(width, height);

    IMDivider popup(popup_name + "_main", DOVertical);
    popup.setZOrdering(10);
    popup_container.setElement(popup);

    IMImage background(white_background);
    background.setSizeX(width);
    background.setSizeY(height);
    background.setColor(vec4(0.0f, 0.0f, 0.0f, 1.0f));

    popup_container.addFloatingElement(background, "background", vec2(0.0f, 0.0f), 9);

    main_container.setPauseBehaviors(true);
    main_container.addFloatingElement(popup_container, popup_name, vec2(background_width * 0.5f - width * 0.5f, background_height * 0.5f - height * 0.5f), 10);

    popup_open = true;
    return @popup;
}

void ClosePopup() {
    if(popup_open) {
        main_container.removeElement(popup_name);
        main_container.setPauseBehaviors(false);
        popup_open = false;
    }
}

string landing_screen_override;
int SettingsOwnerPlayer() {
    return IsPlayerMenuContext() ? GetPlayerMenuLocalIndex() : GetConfigValueInt("controller_menu_owner");
}

bool PersonalSettingsOnly() {
    return !LobbyCanManageLocal();
}

bool CanEditCampaignSettings() {
    return !Online_IsActive() || LobbyIsHost();
}

bool MenuOwnsKeyboard() {
    return SettingsOwnerPlayer() == GetLocalKeyboardPlayerIndex();
}

int SettingsProfilePlayer() {
    if(IsPlayerMenuContext()) return GetPlayerMenuLocalIndex();
    return PersonalSettingsOnly() ? SettingsOwnerPlayer() : GetConfigValueInt("menu_player_config");
}

bool ControllerSettingsAvailable() {
    if(IsPlayerMenuContext()) return IsControllerConnected() || !MenuOwnsKeyboard();
    return IsControllerConnected() || GetLocalPlayerCount() > 1 || PersonalSettingsOnly();
}

void BuildUI(string landing_screen){
    CaptureNumericTextDrafts();
    SetControllerPlayer(controller_player);
    float header_width = 550;
    float header_height = 128;

    ExitRebindMode(false);

    const bool kAnimate = false;

    gui_elements.resize(0);
    category_elements.resize(0);

    IMContainer main_container_local(background_width, background_height);
    @main_container = @main_container_local;

    main_container.setBorderColor(vec4(0,1,0,1));
    float middle_x = main_container.getSizeX() / 2.0f;
    float middle_y = main_container.getSizeY() / 2.0f;
    main_container.setAlignment(CACenter, CACenter);
    IMImage menu_background(settings_sidebar);

    IMImage settings_background(white_background);
    settings_background.setSizeX(1200);
    settings_background.setSizeY(1200);
    settings_background.setZOrdering(0);
    settings_background.setColor(vec4(0,0,0,0.85f));

    if(kAnimate){
        menu_background.addUpdateBehavior(IMMoveIn ( move_in_time, vec2(0, move_in_distance * -1), inQuartTween ), "");
    }
    IMDivider mainDiv( "mainDiv", DOHorizontal );

    menu_background.setSizeY(background_height);
    menu_background.setSizeX(350);
    menu_background.setZOrdering(0);

    mainDiv.setAlignment(CACenter, CACenter);
    mainDiv.setSize(vec2(main_container.getSizeX(), main_container.getSizeY()));

    IMDivider buttons_holder(DOVertical);
    buttons_holder.setSize(vec2(600, mainDiv.getSizeY()));
    buttons_holder.setBorderColor(vec4(1,0,0,1));

    IMImage header_background( brushstroke_background );
    if(kAnimate){
        header_background.addUpdateBehavior(IMMoveIn ( move_in_time, vec2(0, move_in_distance * -1), inQuartTween ), "");
    }
    header_background.scaleToSizeX(header_width);
    header_background.setColor(button_background_color);
    IMDivider header_holder("header_holder", DOHorizontal);
    IMText header_text("Settings", button_font);
    if(kAnimate){
        header_text.addUpdateBehavior(IMMoveIn ( move_in_time, vec2(0, move_in_distance * -1), inQuartTween ), "");
    }
    IMContainer header_container(header_background.getSizeX(), header_background.getSizeY());
    header_container.setElement(header_text);
    header_container.setAlignment(CACenter, CACenter);
    header_text.setZOrdering(3);
    header_container.addFloatingElement(header_background, "background", vec2(0.0f, (0.0f)), 1);
    header_holder.append(header_container);
    buttons_holder.append(header_holder);
    buttons_holder.setBorderColor(vec4(0,0,1,1));

    buttons_holder.append(IMSpacer(DOVertical, 25.0f));

    buttons_holder.setAlignment(CACenter, CACenter);

    mainDiv.append(buttons_holder);
    // Overgrowth Online: Screen (the display, the frame rate, the renderer, anti-aliasing and
    // upscaling) and Graphics (the detail drawn; the old Advanced checkboxes too)
    if(!PersonalSettingsOnly()) {
        AddCategoryButton("Screen", buttons_holder);
        AddCategoryButton("Graphics", buttons_holder);
        AddCategoryButton("Ray Tracing", buttons_holder);
        AddCategoryButton("Audio", buttons_holder);
    }
    if(!PersonalSettingsOnly() || CanEditCampaignSettings()) AddCategoryButton("Game", buttons_holder);
    if((!IsPlayerMenuContext() && !PersonalSettingsOnly()) || MenuOwnsKeyboard()) AddCategoryButton("Controls", buttons_holder);
    controller_connected = ControllerSettingsAvailable();
    if(controller_connected && (!PersonalSettingsOnly() || !MenuOwnsKeyboard())) {
        AddCategoryButton("Gamepad", buttons_holder);
    }

    buttons_holder.append(IMSpacer(DOVertical, 200.0f));

    AddButton("Back", buttons_holder, arrow_icon, button_back, true, 350.0f, 10.0f);

    IMDivider temp_settings_content("settings_content", DOVertical);
    temp_settings_content.setAlignment(CALeft, CACenter);
    @settings_content = @temp_settings_content;

    mainDiv.append(settings_content);

    main_container.addFloatingElement(mainDiv, "menu_content", vec2(0.0f, middle_y - mainDiv.getSizeY() / 2.0f), 0);
    main_container.addFloatingElement(menu_background, "menu_background", vec2(buttons_holder.getSizeX() / 2.0f - menu_background.getSizeX() / 2.0f, middle_y - menu_background.getSizeY() / 2.0f), 0);
    main_container.addFloatingElement(settings_background, "settings_background", vec2(buttons_holder.getSizeX() / 2.0f, middle_y - settings_background.getSizeY() / 2.0f), 0);

    settings_background.addLeftMouseClickBehavior(IMFixedMessageOnClick("close_all_open_menus"), "");
    imGUI.getMain().setElement(@main_container);
    current_screen = "none";
    // (Overgrowth Online, diagnostics: config test_settings_screen opens the menu there)
    if(landing_screen_override.isEmpty() && GetConfigValueString("test_settings_screen") != "") {
        landing_screen_override = GetConfigValueString("test_settings_screen");
    }
    if(landing_screen_override.isEmpty()) {
        SwitchSettingsScreen(landing_screen);
    } else {
        SwitchSettingsScreen(landing_screen_override);
        landing_screen_override = "";
    }
    controller_wraparound = false;
    settings_revision = GetSettingsRevision();
    settings_personal_only = PersonalSettingsOnly();
    settings_match_host = CanEditCampaignSettings();
}

void RestoreCategoryButtons(){
    for(uint i = 0; i < category_elements.size(); i++){
        if(category_elements[i].element_open){
            category_elements[i].DisableElement();
            category_elements[i].element_open = false;
        }
    }
}

void SetActiveCategory(string name){
    for(uint i = 0; i < category_elements.size(); i++){
        if(category_elements[i].name == name){
            category_elements[i].EnableElement();
        }
    }
}

void SwitchSettingsScreen(string screen_name){
    CaptureNumericTextDrafts();
    if(PersonalSettingsOnly() && !(screen_name == "Game" && CanEditCampaignSettings())) screen_name = MenuOwnsKeyboard() ? "Controls" : "Gamepad";
    if(IsPlayerMenuContext() && screen_name == "Controls" && !MenuOwnsKeyboard()) screen_name = "Gamepad";
    if(IsPlayerMenuContext() && screen_name == "Gamepad" && MenuOwnsKeyboard() && !IsControllerConnected()) screen_name = "Controls";
    if(screen_name == "Advanced"){
        screen_name = "Graphics";  // (Overgrowth Online: the old Advanced tab's options are there now)
    }
    // Release controller focus while the old elements still have live parents.
    ClearControllerItems(category_elements.size() + 1);
    settings_content.clear();
    gui_elements.resize(0);

    if(current_screen != screen_name) {
        show_keybinds_offset = 0;
    }

    RestoreCategoryButtons();
    SetActiveCategory(screen_name);

    if(screen_name == "Screen"){
        AddScreenSettingsScreen();
    }
    else if(screen_name == "Graphics"){
        AddGraphicsScreen();
    }
    else if(screen_name == "Ray Tracing"){
        AddRayTracingScreen();
    }
    else if(screen_name == "Audio"){
        AddAudioScreen();
    }
    else if(screen_name == "Game"){
        AddGameScreen();
    }
    else if(screen_name == "Controls"){
        AddInputScreen();
    }
    else if(screen_name == "Gamepad"){
        AddControllerScreen();
    }
    list_created = false;
    current_screen = screen_name;
}

void checkCustomResolution(){
    vec2 currentResolution = vec2(GetConfigValueInt("screenwidth"), GetConfigValueInt("screenheight"));
    array<vec2> possibleResolutions = GetPossibleResolutions();

    bool found = false;
    for(uint i = 0; i < possibleResolutions.size(); ++i) {
        if(possibleResolutions[i] == currentResolution) {
            found = true;
            break;
        }
    }

    // Don't set custom_resolution to true automatically;
    // the user may have entered a possible resolution manually
    if(!found)
        SetConfigValueBool("custom_resolution", true);
}

// Overgrowth Online: the screen. How the frame reaches the display: the monitor, the
// resolution and window, the frame rate, the renderer, anti-aliasing and upscaling
void AddScreenSettingsScreen(){
    current_screen = "Screen";

    array<int> monitorIndices;
    array<string> monitorStrings;

    int monitorCount = GetMonitorCount();
    for(int i = 0; i < monitorCount; ++i) {
        monitorIndices.insertLast(i);
        monitorStrings.insertLast("" + (i + 1));
    }

    if(monitorCount > 1)
        AddDropDown("Target monitor", settings_content, DropdownConfiguration(monitorIndices, monitorStrings, "target_monitor"));

    array<vec2> possible_resolutions = GetPossibleResolutions();
    array<string> resolutions;
    for(uint i = 0; i < possible_resolutions.size(); i++){
        resolutions.insertLast(possible_resolutions[i].x + "x" + possible_resolutions[i].y);
    }
    resolutions.insertLast("Custom");

    array<string> config_names = {"screenwidth", "screenheight"};
    AddResolutionDropDown("Resolution", settings_content, DropdownConfiguration(possible_resolutions, resolutions, config_names ));

    if(GetConfigValueBool("custom_resolution"))
        AddCustomResolutionInput(settings_content);

    array<int> window_type_values = { 0, 1, 2, 3 };
    array<string> window_type_options = { "Windowed", "Fullscreen", "Windowed borderless", "Fullscreen borderless"};
    AddDropDown("Window mode", settings_content, DropdownConfiguration(window_type_values, window_type_options, "fullscreen"));

    // Overgrowth Online: VSync beside the frame rate cap (the cap with VSync off; the
    // menus keep their own)
    array<int> vsync_values = { 0, 1 };
    array<string> vsync_options = { "Off", "On" };
    AddDropDown("VSync", settings_content, DropdownConfiguration(vsync_values, vsync_options, "vsync"));

    array<array<int>> fps_values = { {0, 60}, {1, 30}, {1, 60}, {1, 120}, {1, 144}, {1, 165}, {1, 240} };
    array<string> fps_config_variables = { "limit_fps_in_game", "max_frame_rate" };
    array<string> fps_options = {"Off", "30", "60", "120", "144", "165", "240"};
    AddDropDown("Frame rate limit", settings_content, DropdownConfiguration(fps_values, fps_options, fps_config_variables));

    // Overgrowth Online: the renderer (a Vulkan start that fails comes back on OpenGL)
    array<string> renderer_values = {"gl", "vulkan"};
    array<string> renderer_options = {"OpenGL", "Vulkan"};
    AddDropDown("Renderer", settings_content, DropdownConfiguration( renderer_values, renderer_options, "renderer", "" ), "*");

    // Overgrowth Online: an upscaler, or else the anti-aliasing (TAA or MSAA). The upscalers
    // run on the Vulkan renderer (TAA at full resolution elsewhere): DLSS with an NVIDIA RTX
    // GPU, FSR on any; their modes draw the scene smaller and bring it up to the resolution
    int upscaler = GetConfigValueInt("upscaler");
    bool upscaling = upscaler != 0;
    array<int> upscaler_values = { 0, 1, 2 };
    array<string> upscaler_options = { "Off", "DLSS", "FSR 3.1" };
    AddDropDown("Upscaler", settings_content, DropdownConfiguration(upscaler_values, upscaler_options, "upscaler"));
    if(upscaling) {
        array<int> mode_values = { 0, 1, 2, 3, 4 };
        array<string> mode_options = { upscaler == 2 ? "Native AA" : "Native (DLAA)", "Quality", "Balanced", "Performance", "Ultra Performance" };
        AddDropDown("Upscaling mode", settings_content, DropdownConfiguration(mode_values, mode_options, "upscaler_quality"));
    } else {
        array<array<int>> aa_values = { {1, 0}, {2, 0}, {4, 0}, {8, 0}, {1, 1} };
        array<string> aa_config_variables = { "multisample", "temporal_aa" };
        array<string> aa_options = {"None", "MSAA 2X", "MSAA 4X", "MSAA 8X", "TAA"};
        AddDropDown("Anti-aliasing", settings_content, DropdownConfiguration(aa_values, aa_options, aa_config_variables));
    }
    if(upscaling) {
        AddSlider("Sharpness", settings_content, upscaler == 2 ? "fsr_sharpness" : "dlss_sharpness", 1.0f);
    } else if(GetConfigValueInt("temporal_aa") == 1) {
        AddSlider("Sharpness", settings_content, "temporal_aa_sharpness", 1.0f);
    }

    AddSlider("Brightness", settings_content, "brightness", 2.0f, 200.0f);

    AddLabel("*Requires Restart", settings_content);
    if(GetConfigValueInt("upscaler") != 0 && GetConfigValueString("renderer") != "vulkan") {
        AddNote("Upscalers need the Vulkan renderer: TAA at full resolution on OpenGL", settings_content);
    }
}

// The graphics: the detail drawn (the preset, textures, objects, effects)
void AddGraphicsScreen(){
    current_screen = "Graphics";

    array<string> overall_settings = GetConfigValueOptions("overall");
    AddDropDown("Overall", settings_content, DropdownConfiguration(overall_settings, overall_settings, "overall"));

    array<int> anisotropy_values = { 1, 2, 4, 8 };
    array<string> anisotropy_options = {"none", "2X", "4X", "8X"};
    AddDropDown("Anisotropy", settings_content, DropdownConfiguration( anisotropy_values, anisotropy_options, "anisotropy" ));

    array<int> texture_detail_values = {0, 1, 2, 3};
    array<string> texture_detail_options = {"Full", "1/2", "1/4", "1/8"};
    AddDropDown("Texture Detail", settings_content, DropdownConfiguration( texture_detail_values, texture_detail_options, "texture_reduce"), "*");

    array<array<int>> detail_object_config_values = { {0, 1, 1, 0}, {1, 1, 1 ,0}, {1, 0, 0, 0}, {1, 0, 0, 1} };
    array<string> detail_object_config_variables = { "detail_objects", "detail_object_lowres", "detail_object_disable_shadows", "detail_object_decals" };
    array<string> detail_object_options = {"Off", "Low", "Medium", "High"};
    AddDropDown("Detail objects", settings_content, DropdownConfiguration( detail_object_config_values, detail_object_options, detail_object_config_variables ), "*");

    AddSlider("Motion Blur", settings_content, "motion_blur_amount", 1.0f);

    // (the old Advanced tab's, two columns: the shorter names left, the longer right, a
    // toggle and its refinement side by side; wider columns ran past the panel)
    DualColumns dual_columns("checkboxes_column", 860);
    AddCheckBox("Simple Shadows", dual_columns.left, "simple_shadows", 0.0f);
    AddCheckBox("No reflection capture", dual_columns.right, "no_reflection_capture", 0.0f);
    AddCheckBox("Simple Fog", dual_columns.left, "simple_fog", 0.0f);
    AddCheckBox("Fewer Detail Objects", dual_columns.right, "detail_objects_reduced", 0.0f);
    AddCheckBox("Simple Water", dual_columns.left, "simple_water", 0.0f);
    AddCheckBox("Enable Custom Shaders", dual_columns.right, "custom_level_shaders", 0.0f);
    AddCheckBox("GPU Particle Field", dual_columns.left, "particle_field", 0.0f);
    AddCheckBox("Simple GPU Particles", dual_columns.right, "particle_field_simple", 0.0f);
    AddCheckBox("Depth of field", dual_columns.left, "depth_of_field", 0.0f);
    AddCheckBox("Reduce depth of field", dual_columns.right, "depth_of_field_reduced", 0.0f);
    //AddCheckBox("Use tet mesh lighting", dual_columns.left, "tet_mesh_lighting", 0.0f);
    //AddCheckBox("Use ambient light volumes", dual_columns.right, "light_volume_lighting", 0.0f);
    settings_content.append(dual_columns.main);

    AddLabel("*Requires Restart", settings_content);
    AddNote("Ray-traced shadows, lighting and reflections: the Ray Tracing tab", settings_content);
}

// Overgrowth Online (RT): ray tracing, on a tab of its own. What this PC can do first; a
// preset that sets every effect and how many rays it takes (Custom once one is changed);
// the effects, one line each; one denoiser; then the tuning of the effects that are on;
// and only the problems that stop something working, each with what to change
void AddRayTracingScreen(){
    current_screen = "Ray Tracing";

    const int support = RayTracingSupport();
    const bool vulkan = (support & 1) != 0 || GetConfigValueString("renderer") == "vulkan";
    const bool can_trace = (support & 2) != 0;
    const bool has_nrd = (support & 4) != 0;
    const bool has_rr = (support & 8) != 0;
    if((support & 1) == 0) {
        AddSection(GetConfigValueString("renderer") == "vulkan" ? "Status: Vulkan is set; restart the game to use it"
                                                                  : "Status: needs the Vulkan renderer (Screen tab)", settings_content);
    } else if(!can_trace) {
        AddSection("Status: this GPU can't ray trace", settings_content);
    } else {
        string denoisers = has_nrd ? "NVIDIA NRD" : "the built-in denoiser";
        if(has_rr) {
            denoisers += ", DLSS Ray Reconstruction";
        }
        AddSection("Status: ready (" + denoisers + ")", settings_content);
    }

    // The preset: every effect, and the rays a pixel each takes (sun, AO, lights)
    array<string> preset_names = { "rt_shadows", "rt_ao", "rt_light_shadows", "rt_reflections", "rt_gi", "rt_grass", "rt_shadow_rays", "rt_ao_rays", "rt_light_rays" };
    array<array<int>> preset_values = {
        { 0, 0, 0, 0, 0, 0, 2, 2, 2 },   // Off
        { 1, 1, 0, 0, 0, 0, 1, 1, 1 },   // Low: shadows and AO
        { 1, 1, 1, 1, 0, 1, 1, 2, 1 },   // Medium: + light shadows, reflections, grass
        { 1, 1, 1, 1, 1, 1, 2, 2, 2 },   // High: + global illumination
        { 1, 1, 1, 1, 1, 1, 4, 4, 4 } }; // Ultra: more rays
    array<string> preset_options = { "Off", "Low", "Medium", "High", "Ultra" };
    AddDropDown("Preset", settings_content, DropdownConfiguration(preset_values, preset_options, preset_names));

    AddSection("Effects", settings_content);
    array<int> on_off_values = { 0, 1 };
    array<string> on_off = { "Off", "On" };
    array<string> sun_options = { "Shadow maps", "Ray traced" };
    AddDropDown("Sun shadows", settings_content, DropdownConfiguration(on_off_values, sun_options, "rt_shadows"));
    AddDropDown("Ambient occlusion", settings_content, DropdownConfiguration(on_off_values, on_off, "rt_ao"));
    AddDropDown("Lamp and fire shadows", settings_content, DropdownConfiguration(on_off_values, on_off, "rt_light_shadows"));
    AddDropDown("Reflections", settings_content, DropdownConfiguration(on_off_values, on_off, "rt_reflections"));
    AddDropDown("Global illumination", settings_content, DropdownConfiguration(on_off_values, on_off, "rt_gi"));
    AddDropDown("Grass in ray tracing", settings_content, DropdownConfiguration(on_off_values, on_off, "rt_grass"));

    // One denoiser: NRD, DLSS Ray Reconstruction (NRD's settings kept under it) or the
    // built-in one (no GI)
    array<array<int>> denoiser_values = { { 1, 0 }, { 1, 1 }, { 0, 0 } };
    array<string> denoiser_names = { "rt_nrd", "rt_rr" };
    array<string> denoiser_options = { "NVIDIA NRD", "DLSS Ray Reconstruction", "Built-in" };
    AddDropDown("Denoiser", settings_content, DropdownConfiguration(denoiser_values, denoiser_options, denoiser_names));

    // The tuning, for the effects that are on
    const bool shadows = GetConfigValueBool("rt_shadows");
    const bool ao = GetConfigValueBool("rt_ao");
    const bool gi = GetConfigValueBool("rt_gi");
    const bool grass = GetConfigValueBool("rt_grass");
    if(shadows || ao || gi || grass) {
        AddSection("Tuning", settings_content);
    }
    if(shadows) {
        array<string> soft_values = { "0.53", "1.5", "3" };
        array<string> soft_options = { "Sharp", "Soft", "Softer" };
        AddDropDown("Shadow softness", settings_content, DropdownConfiguration(soft_values, soft_options, "rt_sun_size"));
    }
    if(ao) {
        array<string> range_values = { "0.75", "1.5", "3" };
        array<string> range_options = { "Small", "Medium", "Large" };
        AddDropDown("AO radius", settings_content, DropdownConfiguration(range_values, range_options, "rt_ao_range"));
    }
    if(gi) {
        AddSlider("GI strength", settings_content, "rt_gi_strength", 2.0f, 200.0f);
        AddSlider("Bounce light", settings_content, "rt_gi_bounce", 3.0f, 300.0f);
    }
    if(grass) {
        array<string> grass_values = { "15", "25", "40" };
        array<string> grass_options = { "Near", "Medium", "Far" };
        AddDropDown("Grass distance", settings_content, DropdownConfiguration(grass_values, grass_options, "rt_grass_range"));
    }

    // What stops something working, and what to change
    const bool any = shadows || ao || gi || GetConfigValueBool("rt_light_shadows") || GetConfigValueBool("rt_reflections");
    if(any && !vulkan) {
        AddNote("Set the renderer to Vulkan (Screen tab) for ray tracing", settings_content);
    }
    const bool rr = GetConfigValueBool("rt_rr");
    if(gi && !rr && !GetConfigValueBool("rt_nrd")) {
        AddNote("Global illumination needs NVIDIA NRD or Ray Reconstruction as the denoiser", settings_content);
    }
    if(rr && GetConfigValueInt("upscaler") != 1) {
        AddNote("Ray Reconstruction needs DLSS as the upscaler (Screen tab)", settings_content);
    } else if(rr && (support & 1) != 0 && !has_rr) {
        AddNote("Ray Reconstruction isn't available on this PC: NRD is used", settings_content);
    }
    if(!rr && GetConfigValueBool("rt_nrd") && (support & 1) != 0 && can_trace && !has_nrd) {
        AddNote("NVIDIA NRD isn't available: the built-in denoiser is used", settings_content);
    }
}

// Overgrowth Online: a heading over a group of options, or a status line
void AddSection(string text, IMDivider@ parent){
    IMContainer section_container("section_container" + text, DOHorizontal);
    IMText section(text, button_font_small);
    section_container.setAlignment(CALeft, CACenter);
    section_container.setElement(section);
    section_container.setSizeY(60.0f);
    section_container.setSizeX(800.0f);
    parent.append(section_container);
}

// Overgrowth Online: a line of explanation under the options (no asterisk: not a footnote)
void AddNote(string text, IMDivider@ parent){
    IMContainer note_container("note_container" + text, DOHorizontal);
    IMText note(text, button_font_extra_small);
    note_container.setAlignment(CARight, CACenter);
    note_container.setElement(note);
    note_container.setSizeY(40.0f);
    note_container.setSizeX(725.0f);
    parent.append(note_container);
}

void AddAudioScreen(){
    current_screen = "Audio";
    
    AddSlider("Music volume", settings_content, "music_volume", 1.0f, 100.0f, 0.0f, 0.0f, true);
    AddSlider("Master volume", settings_content, "master_volume", 1.0f, 100.0f, 0.0f, 0.0f, true);
}

void AddGameScreen(){
    current_screen = "Game";

    if(!PersonalSettingsOnly()) {
        AddCheckBox("Show frame rate and frame time", settings_content, "fps_label");
        array<string> locale_values = GetLocaleShortcodes();
        array<string> locale_names = GetLocaleNames();
        AddDropDown("Language", settings_content, DropdownConfiguration( locale_values, locale_names, "language", " (English)" ));
    }
    if(CanEditCampaignSettings()) {
        array<string> difficulty_options = GetConfigValueOptions("difficulty_preset");
        AddDropDown("Difficulty Preset", settings_content, DropdownConfiguration( difficulty_options, difficulty_options, "difficulty_preset" ));
        AddCheckBox("Scale campaign encounters for party size", settings_content, "campaign_mp_scaling");
        AddSlider("Game Speed", settings_content, "global_time_scale_mult", 1.0f, 100.0f, 0.5f, 50.0f);
        AddSlider("Game Difficulty", settings_content, "game_difficulty", 1.0f);
        AddCheckBox("Tutorials", settings_content, "tutorials");
        AddCheckBox("Automatic Ledge Grab", settings_content, "auto_ledge_grab");
    } else {
        settings_content.append(IMText("The host controls match difficulty.", button_font_small));
        settings_content.appendSpacer(12.0f);
        settings_content.append(IMText("Difficulty: " + GetConfigValueString("difficulty_preset"), button_font_small));
        settings_content.append(IMText("Party encounter scaling: " + (GetConfigValueBool("campaign_mp_scaling") ? "On" : "Off"), button_font_small));
        settings_content.appendSpacer(20.0f);
    }
    if(PersonalSettingsOnly()) return;
    array<int> blood_amount_values = { 0, 1, 2};
    array<string> blood_amount_options = {"None", "No dripping", "Full"};
    AddDropDown("Blood Amount", settings_content, DropdownConfiguration( blood_amount_values, blood_amount_options, "blood" ));
    
    array<string> blood_color_values = {"0.4 0 0", "0 0.4 0", "0 0.4 0.4", "0.1 0.1 0.1"};
    array<string> blood_color_options = {"Red", "Green", "Cyan", "Black"};
    AddDropDown("Blood Color", settings_content, DropdownConfiguration( blood_color_values, blood_color_options, "blood_color" ));
    AddCheckBox("Splitscreen", settings_content, "split_screen");
}

void AddInputScreen(){
    current_screen = "Controls";

    AddSlider("Mouse sensitivity", settings_content, "mouse_sensitivity", 2.5f, 200.0f, 0.01f, 1.0f, true);

    DualColumns dual_columns("input_columns", 800);

    AddCheckBox("Invert mouse X", dual_columns.left, "invert_x_mouse_look", 0.0f);
    AddCheckBox("Invert mouse Y", dual_columns.left, "invert_y_mouse_look", 0.0f);

    AddCheckBox("Raw mouse input", dual_columns.right, "use_raw_input", 0.0f);
    if(!PersonalSettingsOnly()) AddCheckBox("Automatic camera", dual_columns.right, "auto_camera", 0.0f);

    settings_content.append(dual_columns.main);

    @key_rebind_container = @IMContainer(0, 0);
    AddKeyBinds(key_rebind_container);
    settings_content.append(key_rebind_container);

    AddBasicButton("Reset to defaults", "reset_bindings", 400, settings_content);
}

void AddControllerScreen(){
    current_screen = "Gamepad";

    array<int> player_values;
    array<string> player_options;
    int player_count = max(1, int(GetLocalPlayerCount()));
    int current_player = SettingsProfilePlayer();
    current_player = min(max(0, current_player), player_count - 1);
    if(!IsPlayerMenuContext()) SetConfigValueInt("menu_player_config", current_player);
    for(int i = 0; i < player_count; ++i) {
        player_values.insertLast(i);
        player_options.insertLast("" + (i + 1));
    }
    if(!PersonalSettingsOnly() && !IsPlayerMenuContext()) {
        AddDropDown("Current player", settings_content, DropdownConfiguration(player_values, player_options, "menu_player_config"));
    }

    string profile = "gamepad_" + current_player;
    DualColumns inversion_columns("gamepad_inversion", 800);
    AddCheckBox("Invert camera X", inversion_columns.left, profile + "_invert_x_look", 0.0f);
    AddCheckBox("Invert camera Y", inversion_columns.right, profile + "_invert_y_look", 0.0f);
    settings_content.append(inversion_columns.main);

    AddSlider("Sensitivity", settings_content, profile + "_look_sensitivity", 5.0f, 200.0f, 0.01f, 1.0f, false);
    AddSlider("Deadzone", settings_content, profile + "_deadzone", 0.5f);

    @key_rebind_container = @IMContainer(0, 0);
    AddControllerBinds(key_rebind_container);
    settings_content.append(key_rebind_container);

    AddBasicButton("Reset to defaults", "reset_bindings", 400, settings_content);
}

const array<string> keyboard_bind_names = {
    "Forward",
    "Backwards",
    "Left",
    "Right",
    "Jump",
    "Crouch",
    "Slow Motion",
    "Equip/sheathe item",
    "Throw/pick-up item",
    "Skip dialogue",
    "Attack",
    "Grab",
    "Walk"
};
const array<string> keyboard_keybinds = {
    "up",
    "down",
    "left",
    "right",
    "jump",
    "crouch",
    "slow",
    "item",
    "drop",
    "skip_dialogue",
    "attack",
    "grab",
    "walk"
};
void AddKeyBinds(IMContainer@ parent) {
    // Left is keybinds, right is scrollbar
    IMDivider key_rebind_horizontal("hori", DOHorizontal);

    IMDivider key_rebind_vertical("vert", DOVertical);
    for(int i = 0; i < kMaxShownKeybinds; ++i) {
        AddKeyRebind(keyboard_bind_names[i + show_keybinds_offset], key_rebind_vertical, "key", keyboard_keybinds[i + show_keybinds_offset]);
    }
    key_rebind_horizontal.append(key_rebind_vertical);

    ConfigureControllerScrollRegion(key_rebind_vertical, show_keybinds_offset, kMaxShownKeybinds, keyboard_keybinds.size());

    AddScrollBar(key_rebind_horizontal, 50.0f, 700.0f, keyboard_keybinds.size(), kMaxShownKeybinds, show_keybinds_offset);

    parent.setElement(key_rebind_horizontal);
}

const array<string> controller_bind_names = {
    "Forward",
    "Backwards",
    "Left",
    "Right",
    "Look Up",
    "Look Down",
    "Look Left",
    "Look Right",
    "Jump",
    "Crouch",
    "Equip/sheathe item",
    "Throw/pick-up item",
    "Skip dialogue",
    "Attack",
    "Grab"
};
const array<string> controller_keybinds = {
    "up",
    "down",
    "left",
    "right",
    "look_up",
    "look_down",
    "look_left",
    "look_right",
    "jump",
    "crouch",
    "item",
    "drop",
    "skip_dialogue",
    "attack",
    "grab"
};
void AddControllerBinds(IMContainer@ parent) {
    // Left is keybinds, right is scrollbar
    IMDivider key_rebind_horizontal("hori", DOHorizontal);

    IMDivider key_rebind_vertical("vert", DOVertical);
    for(int i = 0; i < kMaxShownKeybinds; ++i) {
        AddKeyRebind(controller_bind_names[i + show_keybinds_offset], key_rebind_vertical, "gamepad_" + SettingsProfilePlayer(), controller_keybinds[i + show_keybinds_offset]);
    }
    key_rebind_horizontal.append(key_rebind_vertical);

    ConfigureControllerScrollRegion(key_rebind_vertical, show_keybinds_offset, kMaxShownKeybinds, controller_keybinds.size());

    AddScrollBar(key_rebind_horizontal, 50.0f, 700.0f, controller_keybinds.size(), kMaxShownKeybinds, show_keybinds_offset);

    parent.setElement(key_rebind_horizontal);

}

void SaveUnsavedChanges() {
    bool unsaved_values = false;
    for(uint i = 0; i < gui_elements.size(); i++){
        if(gui_elements[i].unsaved_values){
            unsaved_values = true;
            gui_elements[i].SaveValueToConfig();
        }
    }
    if(unsaved_values)
        ReloadStaticValues();
}

void DiscardUnsavedChanges() {
    numeric_text_drafts.resize(0);
    for(uint i = 0; i < gui_elements.size(); i++){
        if(gui_elements[i].unsaved_values || gui_elements[i].HasTextDraft()){
            gui_elements[i].DiscardValue();
        }
    }
}

void ProcessSettingsMessage(IMMessage@ message){
    if(active_rebind == -1){
        CloseAllOptionMenus();
        if( message.name == "ui_element_clicked" ){
            ToggleUIElement(message.getString(0));
        }
        else if( message.name == "option_changed" ){
            for(uint i = 0; i < gui_elements.size(); i++){
                if(gui_elements[i].name == message.getString(0)){
                    gui_elements[i].SwitchOption(message.getString(1));
                }
            }
            // Only these pages have sections whose presence depends on other
            // options. RefreshAllOptions already updates the remaining widgets.
            if(current_screen == "Screen" || current_screen == "Ray Tracing") SwitchSettingsScreen(current_screen);
        }
        else if( message.name == "close_all_open_menus" ){
            CloseAllOptionMenus();
        }
        else if( message.name == "save_unsaved_changes" ){
            SaveUnsavedChanges();
            SwitchSettingsScreen(current_screen);
        }
        else if( message.name == "discard_unsaved_changes" ){
            DiscardUnsavedChanges();
        }
        else if( message.name == "slider_deactivate" ){
            //Print("deactivate slider check\n");
            if(!checking_slider_movement){
                active_slider = -1;
            }
        }
        else if( message.name == "slider_activate" ){
            //Print("activate slider check\n");
            old_mouse_pos = imGUI.guistate.mousePosition;
            active_slider = message.getInt(0);
        }
        else if( message.name == "slider_move_check" ){
            /*continues_updating_element = message.getInt(0);*/
        }
        else if( message.name == "switch_category" ){
            SwitchSettingsScreen(message.getString(0));
        }
        else if( message.name == "rebind_activate" && allow_rebind ){
            if(controller_active) {
                if(current_screen == "Controls")
                    EnterRebindMode(message.getInt(0), true);
                else
                    EnterRebindMode(message.getInt(0), false);
            } else {
                // Need to check the mouse. If not, rebinding LMB might trigger another rebind_active
                array<MousePress> mouse_inputs = GetRawMouseInputs();
                if(mouse_inputs.size() > 0 && int(mouse_inputs[mouse_inputs.size() - 1].s_id) != mouse_sequence_id) {
                    if(current_screen == "Controls")
                        EnterRebindMode(message.getInt(0), true);
                    else
                        EnterRebindMode(message.getInt(0), false);
                }
            }
        }
        else if( message.name == "reset_bindings" ){
            if(current_screen == "Controls") {
                for(uint i = 0; i < keyboard_keybinds.size(); ++i) {
                    ResetBinding("key", keyboard_keybinds[i]);
                }
            } else if(current_screen == "Gamepad") {
                for(uint i = 0; i < controller_keybinds.size(); ++i) {
                    ResetBinding("gamepad_" + SettingsProfilePlayer(), controller_keybinds[i]);
                }
            }
            RefreshAllOptions();
        }
        else if( message.name == "back" ){
            //Log(info,"Received back message");
            bool editing_text = false;
            for(uint i = 0; i < gui_elements.length(); ++i) editing_text = editing_text || gui_elements[i].HasTextDraft();
            if(editing_text) {
                DiscardUnsavedChanges();
            } else if(OptionMenuOpen()){
                CloseAllOptionMenus();
            }else{
                DiscardUnsavedChanges();
                imGUI.receiveMessage(IMMessage("Back"));
            }
        }
        else if(message.name == "enable_element"){
            if(message.numStrings() > 0) {
                string target_name = message.getString(0);

                for(uint i = 0; i < gui_elements.size(); ++i) {
                    if(gui_elements[i].name == target_name) {
                        gui_elements[i].EnableElement();
                        break;
                    }
                }
            }
        }
        else if(message.name == "shift_menu" || message.name == "controller_scroll"){
            if(current_screen == "Controls" || current_screen == "Gamepad") {
                int offset = message.getInt(0);
                show_keybinds_offset += offset;
                show_keybinds_offset = max(0, show_keybinds_offset);
                if(current_screen == "Controls") {
                    show_keybinds_offset = min(show_keybinds_offset, keyboard_keybinds.size() - kMaxShownKeybinds);
                } else {
                    show_keybinds_offset = min(show_keybinds_offset, controller_keybinds.size() - kMaxShownKeybinds);
                }
                SwitchSettingsScreen(current_screen);
                if(message.name == "controller_scroll") FocusControllerScrollEdge(controller_scroll_region, offset);
            }
        }
        else if(message.name.findFirst("menu_player") == 0 && !IsPlayerMenuContext()) {
            int player = parseInt(message.name.substr(11));
            SetConfigValueInt("menu_player_config", player);
            if(settings_content !is null && current_screen == "Gamepad") SwitchSettingsScreen(current_screen);
        }
        else{
            Log( info, "Unknown processMessage " + message.getString(0) );
        }
    } else {
        if(message.name == "apply_bind") {
            ApplyBind();
        }
        else if(message.name == "cancel_bind") {
            ExitRebindMode(false);
        }
        else if(message.name == "open_menu") {
            ExitRebindMode(false);
        }
        else if(message.name == "close_all_open_menus" || message.name == "back"){
            ExitRebindMode(false);
            DiscardUnsavedChanges();
            CloseAllOptionMenus();
        }
        else{
            Log( info, "Unknown processMessage " + message.getString(0) );
        }
    }
    settings_revision = GetSettingsRevision();
}

void ApplyBind() {
    KeyRebind@ element = cast<KeyRebind>(gui_elements[active_rebind]);
    if(potential_rebind_controller_button != kInvalidControllerButton) {
        SetControllerBindingValue(element.binding_category, element.binding, potential_rebind_controller_button);
    } else if(potential_rebind_scancode != kInvalidKeyboardButton) {
        SetKeyboardBindingValue(element.binding_category, element.binding, potential_rebind_scancode);
    } else if(potential_rebind_mouse_button != kInvalidMouseButton) {
        SetMouseBindingValue(element.binding_category, element.binding, potential_rebind_mouse_button);
    } else if(potential_rebind_scroll_dir != SCROLL_NONE) {
        if(potential_rebind_scroll_dir == SCROLL_UP) {
            SetMouseBindingValue(element.binding_category, element.binding, "mousescrollup");
        } else if(potential_rebind_scroll_dir == SCROLL_DOWN) {
            SetMouseBindingValue(element.binding_category, element.binding, "mousescrolldown");
        } else if(potential_rebind_scroll_dir == SCROLL_LEFT) {
            SetMouseBindingValue(element.binding_category, element.binding, "mousescrollleft");
        } else if(potential_rebind_scroll_dir == SCROLL_RIGHT) {
            SetMouseBindingValue(element.binding_category, element.binding, "mousescrollright");
        } else {
            Log(error, "Unknown scroll dir");
        }
    }
    element.PutCurrentValue();
    ExitRebindMode(true);
}

void UpdateSettings(bool auto_exit_rebind = false){
    if(settings_personal_only != PersonalSettingsOnly() || settings_match_host != CanEditCampaignSettings()) {
        // A promoted leader gains client settings; the former leader loses
        // them immediately, even when both settings menus were already open.
        ExitRebindMode(false);
        DiscardUnsavedChanges();
        CloseAllOptionMenus();
        settings_personal_only = PersonalSettingsOnly();
        settings_match_host = CanEditCampaignSettings();
        imGUI.receiveMessage(IMMessage("rebuild_settings"));
        return;
    }
    if(settings_revision != GetSettingsRevision() && active_rebind == -1 && active_slider == -1 &&
       !popup_open && !OptionMenuOpen()) {
        // Each personal menu owns its widgets, while configuration belongs to
        // the client. Observe changes without writing any cached values back.
        bool editing = false;
        for(uint i = 0; i < gui_elements.size(); ++i) editing = editing || gui_elements[i].unsaved_values;
        if(!editing) {
            if(current_screen == "Screen" || current_screen == "Ray Tracing" || current_screen == "Game") SwitchSettingsScreen(current_screen);
            else RefreshAllOptions();
            settings_revision = GetSettingsRevision();
        }
    }
    if(!allow_rebind) {
        allow_rebind = !GetInputDown(controller_player, "menu_return");
        if(controller_active) {
            controller_paused = !allow_rebind;
        } else {
            controller_paused = false;
        }
    }
    if(active_rebind != -1) {
        if(current_screen == "Controls") {
            UpdateKeyRebinding(auto_exit_rebind);
        } else {
            UpdateControllerRebinding(auto_exit_rebind);
        }
    } else {
        if(MenuOwnsKeyboard() && GetInputDown(controller_player, "mousescrollup")){
            imGUI.receiveMessage(IMMessage( "shift_menu", -1));
        } else if(MenuOwnsKeyboard() && GetInputDown(controller_player, "mousescrolldown")){
            imGUI.receiveMessage(IMMessage( "shift_menu", 1));
        }
        uint revision_before_updates = GetSettingsRevision();
        UpdateMovingSlider();
        for(uint i = 0; i < gui_elements.size(); i++) {
            gui_elements[i].Update();
        }
        if(revision_before_updates != GetSettingsRevision()) settings_revision = GetSettingsRevision();
    }
    if(controller_connected != ControllerSettingsAvailable()) {
        if(current_screen == "Gamepad") {
            landing_screen_override = "Controls";
        } else {
            landing_screen_override = current_screen;
        }
        imGUI.receiveMessage(IMMessage("rebuild_settings"));
    }
}

void EnterRebindMode(int rebind_index, bool keyboard) {
    potential_rebind_scancode = kInvalidKeyboardButton;
    potential_rebind_mouse_button = kInvalidMouseButton;
    potential_rebind_controller_button = kInvalidControllerButton;
    potential_rebind_scroll_dir = SCROLL_NONE;

    controller_paused = true;

    active_rebind = rebind_index;
    rebind_key = gui_elements[active_rebind].name;
    array<KeyboardPress> keyboard_inputs = GetRawKeyboardInputs();
    if(keyboard_inputs.size() > 0) {
        keyboard_sequence_id = keyboard_inputs[keyboard_inputs.size()-1].s_id;
    } else {
        keyboard_sequence_id = -1;
    }
    array<MousePress> mouse_inputs = GetRawMouseInputs();
    if(mouse_inputs.size() > 0) {
        mouse_sequence_id = mouse_inputs[mouse_inputs.size()-1].s_id;
    } else {
        mouse_sequence_id = -1;
    }
    array<ControllerPress> controller_inputs = GetRawControllerInputs(controller_player);
    if(controller_inputs.size() > 0) {
        controller_sequence_id = controller_inputs[controller_inputs.size()-1].s_id;
    } else {
        controller_sequence_id = -1;
    }

    const float POPUP_WIDTH = 1000;
    const float POPUP_HEIGHT = 500;

    IMDivider@ popup = OpenPopup(POPUP_WIDTH, POPUP_HEIGHT);

    popup.append(IMText("Rebinding " + rebind_key, button_font));

    @rebind_text = @IMText("", button_font);
    popup.append(rebind_text);
    if(keyboard)
        @hint_text = @IMText("Press key to bind, or escape to cancel", button_font);
    else
        @hint_text = @IMText("Press key to bind, or back to cancel", button_font);
    popup.append(hint_text);

    if(keyboard) {
        @apply_label = @IMText("Apply", button_font);
        apply_label.setColor(vec4(1.0f, 1.0f, 1.0f, 0.5f));
        @cancel_label = @IMText("Cancel", button_font);
        cancel_label.addMouseOverBehavior(text_color_mouse_over, "");
        cancel_label.addLeftMouseClickBehavior(IMFixedMessageOnClick("cancel_bind"), "");

        IMDivider buttons("buttons", DOHorizontal);

        popup.appendSpacer(100.0f);
        popup.append(buttons);
        buttons.append(apply_label);
        buttons.appendSpacer(200.0f);
        buttons.append(cancel_label);
    }

    imGUI.receiveMessage(IMMessage("capture_input"));
}

void ExitRebindMode(bool key_rebound) {
    potential_rebind_scancode = kInvalidKeyboardButton;
    potential_rebind_mouse_button = kInvalidMouseButton;
    potential_rebind_controller_button = kInvalidControllerButton;
    potential_rebind_scroll_dir = SCROLL_NONE;
    active_rebind = -1;
    rebind_key = "";
    imGUI.receiveMessage(IMMessage("release_input"));
    ClosePopup();
    if(!key_rebound) {
        controller_paused = false;
    }
    allow_rebind = false;
}

void ChangeKeybindLabel() {
    if(potential_rebind_mouse_button == -1 && potential_rebind_scancode == -1 && potential_rebind_scroll_dir == SCROLL_NONE) {
        apply_label.addMouseOverBehavior(text_color_mouse_over, "");
        apply_label.addLeftMouseClickBehavior(IMFixedMessageOnClick(IMMessage("apply_bind")), "");
        apply_label.setColor(button_font.color);
        hint_text.setText("Press key again or apply");
    }
}

void UpdateKeyRebinding(bool auto_exit_rebind){
    array<KeyboardPress> keyboard_inputs = GetRawKeyboardInputs();
    if(keyboard_inputs.size() > 0){
        KeyboardPress last_pressed = keyboard_inputs[keyboard_inputs.size()-1];
        if( last_pressed.s_id != uint16(keyboard_sequence_id) ) {
            keyboard_sequence_id = last_pressed.s_id;
            if(!apply_label.isMouseOver() && !cancel_label.isMouseOver()) {
                // 27 == escape
                if(last_pressed.keycode == 27 && auto_exit_rebind){
                    ExitRebindMode(false);
                } else if(last_pressed.scancode == uint(potential_rebind_scancode)) {
                    ApplyBind();
                } else {
                    ChangeKeybindLabel();
                    rebind_text.setText("to " + GetLocaleStringForScancode(last_pressed.scancode));
                    potential_rebind_scancode = last_pressed.scancode;
                    potential_rebind_mouse_button = -1;
                    potential_rebind_scroll_dir = SCROLL_NONE;
                }
            }
        }
    }
    array<MousePress> mouse_inputs = GetRawMouseInputs();
    if(mouse_inputs.size() > 0){
        MousePress last_pressed = mouse_inputs[mouse_inputs.size()-1];
        if( last_pressed.s_id != uint16(mouse_sequence_id)) {
            mouse_sequence_id = uint16(last_pressed.s_id);
            if(!apply_label.isMouseOver() && !cancel_label.isMouseOver()) {
                if(last_pressed.button == potential_rebind_mouse_button) {
                    ApplyBind();
                } else {
                    ChangeKeybindLabel();
                    rebind_text.setText("to " + GetStringForMouseButton(last_pressed.button));
                    potential_rebind_scancode = -1;
                    potential_rebind_mouse_button = last_pressed.button;
                    potential_rebind_scroll_dir = SCROLL_NONE;
                }
            }
        }
    }
    if(GetInputDown(controller_player, "mousescrollup")){
        if(potential_rebind_scroll_dir == SCROLL_UP) {
            ApplyBind();
        }
        ChangeKeybindLabel();
        rebind_text.setText("to scroll up");
        potential_rebind_scancode = -1;
        potential_rebind_mouse_button = -1;
        potential_rebind_scroll_dir = SCROLL_UP;
    } else if(GetInputDown(controller_player, "mousescrolldown")){
        if(potential_rebind_scroll_dir == SCROLL_DOWN) {
            ApplyBind();
        }
        ChangeKeybindLabel();
        rebind_text.setText("to scroll down");
        potential_rebind_scancode = -1;
        potential_rebind_mouse_button = -1;
        potential_rebind_scroll_dir = SCROLL_DOWN;
    } else if(GetInputDown(controller_player, "mousescrollleft")){
        if(potential_rebind_scroll_dir == SCROLL_LEFT) {
            ApplyBind();
        }
        ChangeKeybindLabel();
        rebind_text.setText("to scroll left");
        potential_rebind_scancode = -1;
        potential_rebind_mouse_button = -1;
        potential_rebind_scroll_dir = SCROLL_LEFT;
    } else if(GetInputDown(controller_player, "mousescrollright")){
        if(potential_rebind_scroll_dir == SCROLL_RIGHT) {
            ApplyBind();
        }
        ChangeKeybindLabel();
        rebind_text.setText("to scroll right");
        potential_rebind_scancode = -1;
        potential_rebind_mouse_button = -1;
        potential_rebind_scroll_dir = SCROLL_RIGHT;
    }
}

// Used to make sure the user releases the thumbstick, and then moves it back
// again to confirm the bind
bool confirmed = false;

void UpdateControllerRebinding(bool auto_exit_rebind){
    array<ControllerPress> controller_inputs = GetRawControllerInputs(controller_player);
    if(controller_inputs.size() > 0) {
        uint start_index = 0;
        for(start_index = controller_inputs.size() - 1; start_index >= 0; start_index--) {
            if(controller_inputs[start_index].s_id <= uint32(controller_sequence_id)) {
                break;
            }
        }
        for(; start_index < controller_inputs.size(); start_index++) {
            ControllerPress last_pressed = controller_inputs[start_index];
            if( last_pressed.s_id != uint32(controller_sequence_id)) {
                controller_sequence_id = last_pressed.s_id;
                if(last_pressed.depth > 0.5f) {
                    if(last_pressed.input == BACK && auto_exit_rebind) {
                        ExitRebindMode(false);
                        break;
                    } else if(last_pressed.input == potential_rebind_controller_button && confirmed) {
                        ApplyBind();
                        break;
                    } else {
                        confirmed = false;
                    }

                    if(potential_rebind_controller_button == kInvalidControllerButton) {
                        hint_text.setText("Repeat input to apply");
                    }
                    rebind_text.setText("to " + GetStringForControllerInput(last_pressed.input));

                    potential_rebind_controller_button = last_pressed.input;

                    // Buttons are automatically confirmed, since they are eiter on
                    // or off. The next time an input event is received from a
                    // button it has been released and then pressed again.
                    switch(last_pressed.input) {
                        case ControllerInput::L_STICK_XN:
                        case ControllerInput::L_STICK_XP:
                        case ControllerInput::L_STICK_YN:
                        case ControllerInput::L_STICK_YP:
                        case ControllerInput::R_STICK_XN:
                        case ControllerInput::R_STICK_XP:
                        case ControllerInput::R_STICK_YN:
                        case ControllerInput::R_STICK_YP:
                        case ControllerInput::L_TRIGGER:
                        case ControllerInput::R_TRIGGER:
                            break;
                        default:
                            confirmed = true;
                            break;
                    }
                } else if(last_pressed.input == potential_rebind_controller_button) {
                    confirmed = true;
                }
            }
        }
    }
}
