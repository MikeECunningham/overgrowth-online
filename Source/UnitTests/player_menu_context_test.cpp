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
        ensure("pointer is outside all clickable content", PlayerMenuUI::MouseTopLeft().x() < 0);
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
}
