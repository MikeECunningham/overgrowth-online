#include <GUI/player_menu_context.h>
#include <GUI/IMUI/im_support.h>
#include <Internal/config.h>
#include <tut/tut.hpp>

namespace tut {
struct datamenuscope {
    Config saved = config;
    ScreenMetrics saved_metrics = screenMetrics;
    ~datamenuscope() { config = saved; screenMetrics = saved_metrics; }
};
typedef test_group<datamenuscope> tg_menuscope;
tg_menuscope test_group_menuscope("PlayerMenuScope");
typedef tg_menuscope::object menuscope;

template <> template <>
void menuscope::test<1>() {
    set_test_name("nested personal menu calls restore viewport metrics and settings ownership");
    // Unit tests do not create a window; use an explicit outer scale rather
    // than the startup GUI singleton's not-yet-computed display metrics.
    screenMetrics.GUItoScreenXScale = 0.5f;
    const Config saved = config;
    const ScreenMetrics original = screenMetrics;
    config.GetRef("controller_menu_owner") = 7;
    config.GetRef("menu_player_config") = 9;
    PlayerMenuUI::Context first;
    first.input_id = 6; first.local_index = 4; first.x = 640;
    first.width = 640; first.height = 720;
    {
        PlayerMenuUI::Scope outer(first);
        ensure_equals("viewport width is local", PlayerMenuUI::Width(), 640);
        ensure_equals("viewport height is local", PlayerMenuUI::Height(), 720);
        ensure_equals("settings belong to logical player", config["controller_menu_owner"].toNumber<int>(), 4);
        ensure_equals("physical input ID is independent", PlayerMenuUI::Current()->input_id, 6);
        ensure("gamepad menu ignores mouse", !PlayerMenuUI::AllowsMouse());
        vec2 pointer;
        ensure("pointer is outside all clickable content", !PlayerMenuUI::MouseTopLeft(pointer, false));
        const ScreenMetrics outer_metrics = screenMetrics;
        PlayerMenuUI::Context second;
        second.input_id = 0; second.local_index = 0;
        second.width = 1280; second.height = 720; second.mouse_enabled = true;
        {
            PlayerMenuUI::Scope inner(second);
            ensure_equals("inner settings owner", config["controller_menu_owner"].toNumber<int>(), 0);
            ensure_equals("inner viewport width", PlayerMenuUI::Width(), 1280);
        }
        ensure_equals("outer settings restored", config["controller_menu_owner"].toNumber<int>(), 4);
        ensure_equals("outer viewport restored", PlayerMenuUI::Width(), 640);
        ensure_equals("outer scale restored", screenMetrics.GUItoScreenXScale, outer_metrics.GUItoScreenXScale);
    }
    const bool restored = !PlayerMenuUI::Current() &&
        config["controller_menu_owner"].toNumber<int>() == 7 && config["menu_player_config"].toNumber<int>() == 9 &&
        screenMetrics.GUItoScreenXScale == original.GUItoScreenXScale;
    config = saved;
    ensure("global UI state restored on return", restored);
}
template <> template <>
void menuscope::test<2>() {
    set_test_name("temporary menu ownership does not dirty persistent configuration");
    config.has_changed_since_save = false;
    PlayerMenuUI::Context context;
    {
        PlayerMenuUI::Scope scope(context);
        ensure("opening and drawing a menu does not require a config save", !config.HasChangedSinceLastSave());
    }
    ensure("restoring ownership does not require a save", !config.HasChangedSinceLastSave());
    {
        PlayerMenuUI::Scope scope(context);
        config.GetRef("gamepad_0_invert_x_look") = true;
    }
    ensure("a real settings edit remains dirty after scope exit", config.HasChangedSinceLastSave());
}
template <> template <>
void menuscope::test<3>() {
    set_test_name("graphics values are shared between personal menus while ownership stays private");
    PlayerMenuUI::Context first, second;
    first.local_index = 0;
    second.local_index = 1;
    {
        PlayerMenuUI::Scope scope(first);
        config.GetRef("simple_fog") = true;
    }
    {
        PlayerMenuUI::Scope scope(second);
        ensure("another screen observes the same graphics setting", config["simple_fog"].toBool());
        ensure_equals("another screen keeps its own settings owner", config["menu_player_config"].toNumber<int>(), 1);
    }
    ensure("graphics edits survive exiting either menu", config["simple_fog"].toBool());
}
template <> template <>
void menuscope::test<4>() {
    set_test_name("a held mouse drag crosses a split edge without jumping across either axis");
    PlayerMenuUI::Context view;
    view.width = 640; view.height = 360; view.y = 360; view.mouse_enabled = true;
    vec2 pointer(130, 410);
    ensure("outside pointer cannot start hover or drag", !PlayerMenuUI::LocalMousePosition(view, 720, pointer, false));
    pointer = vec2(130, 410);
    ensure("active drag gets the split boundary", PlayerMenuUI::LocalMousePosition(view, 720, pointer, true));
    ensure_equals("crossing vertically retains X", pointer.x(), 130.0f);
    ensure_equals("Y stops just outside the viewport", pointer.y(), 360.0f);
    pointer = vec2(-20, 305);
    ensure("left edge tracks a held drag", PlayerMenuUI::LocalMousePosition(view, 720, pointer, true));
    ensure_equals("X stops just outside the left edge", pointer.x(), -1.0f);
    ensure_equals("Y follows the mouse", pointer.y(), 305.0f);
    view.mouse_enabled = false;
    ensure("another player's menu cannot inherit the drag", !PlayerMenuUI::LocalMousePosition(view, 720, pointer, true));
    view.mouse_enabled = true;
    view.height = 0;
    ensure("an empty viewport cannot capture a drag", !PlayerMenuUI::LocalMousePosition(view, 720, pointer, true));
}
}
