#include <Internal/settings_changes.h>
#include <Graphics/graphics.h>
#include <tut/tut.hpp>

extern bool g_detail_objects_reduced;
extern bool g_detail_objects_reduced_dirty;

namespace tut {
struct datasettingschanges {};
typedef test_group<datasettingschanges> tg_settingschanges;
tg_settingschanges test_group_settingschanges("SettingsChanges");
typedef tg_settingschanges::object settingschanges;

template <> template <>
void settingschanges::test<1>() {
    set_test_name("personal controls and audio never request graphics reconstruction");
    SettingsChanges::Pending pending;
    for (const char* key : {"gamepad_17_invert_y_look", "gamepad_17[jump]", "key[jump]",
                           "invert_y_mouse_look", "use_raw_input", "mouse_sensitivity",
                           "music_volume", "master_volume", "menu_player_config", "rt_ao"}) {
        pending.Changed(key);
        ensure("runtime setting must not rebuild graphics", !pending.TakeGraphicsReload());
    }
    ensure_equals("changes remain observable by other menus", pending.Revision(), uint32_t(10));
}
template <> template <>
void settingschanges::test<2>() {
    set_test_name("multi-key presets apply graphics once and preserve later real changes");
    SettingsChanges::Pending pending;
    pending.Changed("screenwidth");
    pending.Changed("screenheight");
    pending.Changed("multisample");
    ensure("combined settings require one application", pending.TakeGraphicsReload());
    ensure("duplicate apply is skipped", !pending.TakeGraphicsReload());
    pending.Changed("gamepad_5_deadzone");
    ensure("a personal edit cannot apply stale graphics work", !pending.TakeGraphicsReload());
    pending.Changed("custom_mod_render_setting");
    ensure("unknown settings retain original reload semantics", pending.TakeGraphicsReload());
    ensure("graphics are shared client settings", SettingsChanges::SharedClientSetting("simple_shadows"));
    ensure("audio is a shared client setting", SettingsChanges::SharedClientSetting("music_volume"));
    ensure("bindings belong to their player", !SettingsChanges::SharedClientSetting("gamepad_5[jump]"));
}
template <> template <>
void settingschanges::test<3>() {
    set_test_name("unchanged detail density does not regenerate grass");
    const bool value = g_detail_objects_reduced;
    const bool dirty = g_detail_objects_reduced_dirty;
    g_detail_objects_reduced_dirty = false;
    Graphics::Instance()->SetDetailObjectsReduced(value);
    ensure("unchanged value stays clean", !g_detail_objects_reduced_dirty);
    Graphics::Instance()->SetDetailObjectsReduced(!value);
    ensure("real density changes still invalidate grass", g_detail_objects_reduced_dirty);
    Graphics::Instance()->SetDetailObjectsReduced(value);
    g_detail_objects_reduced_dirty = dirty;
}
template <> template <>
void settingschanges::test<4>() {
    set_test_name("campaign match settings are shared runtime state, without graphics rebuilds");
    SettingsChanges::Pending pending;
    for (const char* key : {"difficulty_preset", "game_difficulty", "global_time_scale_mult", "tutorials",
                           "auto_ledge_grab", "campaign_mp_scaling", "campaign_mp_nightmare"}) {
        ensure("host authority covers every campaign control", SettingsChanges::CampaignMatchSetting(key));
        ensure("personal menus cannot own match settings", SettingsChanges::SharedClientSetting(key));
        pending.Changed(key);
        ensure("difficulty cannot rebuild grass or render targets", !pending.TakeGraphicsReload());
    }
    ensure("audio stays client-local", !SettingsChanges::CampaignMatchSetting("master_volume"));
    ensure("graphics stay client-local", !SettingsChanges::CampaignMatchSetting("renderer"));
    ensure("bindings stay personal", !SettingsChanges::CampaignMatchSetting("gamepad_5[jump]"));
}
}
