// Local input capacity, binding inheritance and device ownership regressions.
#include <UserInput/input.h>
#include <Internal/config.h>
#include <Graphics/Cursor.h>

#include <tut/tut.hpp>

namespace tut {
struct datalocalinput {
    Config saved_config;
    Input input;
    datalocalinput() : saved_config(config) {
        config.GetRef("gamepad_deadzone") = 0.075f;
        config.GetRef("gamepad_look_sensitivity") = 1.25f;
    }
    ~datalocalinput() {
        config = saved_config;
    }
};

typedef test_group<datalocalinput> tg_localinput;
tg_localinput test_group_localinput("LocalInput");
typedef tg_localinput::object localinput;

template <>
template <>
void localinput::test<1>() {
    set_test_name("local slots grow past four without a fixed player ceiling");
    input.SetUpForXPlayers(300);
    for (int i = 0; i < 300; ++i) {
        PlayerInput* slot = input.GetController(i);
        ensure("local slot exists and is enabled", slot && slot->enabled && !slot->remote_controlled);
    }
    ensure("no implicit extra player", input.GetController(300) == NULL);
    input.ProcessController(-1, 0.01f);
    input.ProcessController(300, 0.01f);
    input.SetUpForXPlayers(1);
    ensure("P1 remains enabled", input.GetController(0)->enabled);
    ensure("unused local slot is disabled", !input.GetController(299)->enabled);
}

template <>
template <>
void localinput::test<2>() {
    set_test_name("online remote slots stay separate and can later become offline locals");
    input.SetUpForXPlayers(1);
    const int remote = input.AllocateRemotePlayerInput();
    ensure_equals("existing first remote ID", remote, 4);
    input.GetController(remote)->key_down["quit"].count = 1;
    input.SetUpForXPlayers(1);
    ensure("active remote preserved", input.GetController(remote)->remote_controlled);
    ensure_equals("remote cannot open local pause menu", input.PlayerOpenedMenu(), -1);
    input.SetUpForXPlayers(5);
    ensure("offline P5 can reuse the old remote slot", !input.GetController(4)->remote_controlled);
    ensure("offline P5 enabled", input.GetController(4)->enabled);
    ensure_equals("stale remote menu input cleared", input.PlayerOpenedMenu(), -1);
}

template <>
template <>
void localinput::test<3>() {
    set_test_name("multi-digit gamepad profiles inherit defaults and retain overrides");
    config.GetRef("gamepad[jump]") = "rightshoulder";
    config.GetRef("gamepad[attack]") = "leftshoulder";
    config.GetRef("gamepad_11[jump]") = "a";
    input.SetFromConfig(config);
    input.SetUpForXPlayers(12);
    ensure_equals("per-player override", input.GetBindingValue("gamepad_11", "jump"), std::string("a"));
    ensure_equals("missing bind inherits", input.GetBindingValue("gamepad_11", "attack"), std::string("leftshoulder"));
    ensure_equals("new player inherits", input.GetBindingValue("gamepad_10", "jump"), std::string("rightshoulder"));
    ensure("sensitivity inherits", config["gamepad_11_look_sensitivity"].toNumber<float>() > 1.0f);
    config.GetRef("gamepad[jump]") = "b";
    input.SetFromConfig(config);
    ensure_equals("default changes propagate", input.GetBindingValue("gamepad_10", "jump"), std::string("b"));
    ensure_equals("explicit override survives reload", input.GetBindingValue("gamepad_11", "jump"), std::string("a"));
}

template <>
template <>
void localinput::test<4>() {
    set_test_name("growing local roster preserves pad ownership and high slots receive input");
    Input::JoystickMap& pads = input.GetOpenJoysticks();
    pads[1]->player_input = 0;
    input.SetUpForXPlayers(2);
    ensure_equals("keyboard and one pad", pads[1]->player_input, 1);
    input.SetUpForXPlayers(3);
    ensure_equals("adding player preserves existing pad", pads[1]->player_input, 1);
    pads[2]->player_input = -1;
    input.SetUpForXPlayers(4);
    ensure_equals("new pad gets next vacant slot", pads[2]->player_input, 2);
    ensure_equals("first pad remains P2", pads[1]->player_input, 1);
    input.SetUpForXPlayers(12);
    pads[3]->player_input = 11;
    pads[3]->joystick.buttons_down_["up"] = 1.0f;
    input.ProcessController(11, 1.0f / 120.0f);
    ensure("P12 receives its own pad", input.GetController(11)->key_down["up"].depth > 0.5f);
    ensure("P2 did not receive P12 input", input.GetController(1)->key_down["up"].depth == 0.0f);
}
template <>
template <>
void localinput::test<5>() {
    set_test_name("simulated input targets one high-index local player only");
    input.SetUpForXPlayers(12);
    Input::InjectedKey key;
    key.steps_left = 2;
    input.InjectedKeysForController(11)["up"] = key;
    float* look = input.InjectedLookForController(11);
    look[0] = 3.0f;
    look[1] = -2.0f;
    input.ProcessController(11, 0.01f);
    ensure("P12 receives movement", input.GetController(11)->key_down["up"].depth > 0.5f);
    ensure_equals("P12 receives look", input.GetController(11)->key_down["look_right"].depth, 3.0f);
    ensure("P1 receives none", input.GetController(0)->key_down["up"].depth == 0.0f);
    input.ProcessController(11, 0.01f);
    ensure("held input expires", input.InjectedKeysForController(11).empty());
    ensure_equals("look is applied once", input.GetController(11)->key_down["look_right"].depth, 0.0f);
    input.ProcessController(11, 0.01f);
    ensure_equals("released input clears", input.GetController(11)->key_down["up"].depth, 0.0f);
    ensure("legacy P1 injection map", &input.InjectedKeysForController(0) == &input.injected_keys_);
    ensure("legacy P1 injection look", input.InjectedLookForController(0) == input.injected_look_);
}
template <>
template <>
void localinput::test<6>() {
    set_test_name("camera inversion is per gamepad and leaves movement/menu input unchanged");
    input.SetUpForXPlayers(12);
    Input::JoystickMap& pads = input.GetOpenJoysticks();
    pads[1]->player_input = 11;
    pads[1]->joystick.look_sensitivity_ = 2.0f;
    pads[1]->joystick.buttons_down_["look_right"] = 0.75f;
    pads[1]->joystick.buttons_down_["look_down"] = 0.5f;
    pads[1]->joystick.buttons_down_["up"] = 1.0f;
    pads[1]->joystick.buttons_down_["menu_right"] = 1.0f;
    config.GetRef("gamepad_11_invert_x_look") = true;
    input.ProcessController(11, 0.01f);
    ensure_equals("X inverted once, after sensitivity", input.GetController(11)->key_down["look_left"].depth, 1.5f);
    ensure_equals("Y retains normal direction", input.GetController(11)->key_down["look_down"].depth, 1.0f);
    ensure_equals("movement stays forward", input.GetController(11)->key_down["up"].depth, 1.0f);
    ensure_equals("menu direction stays right", input.GetController(11)->key_down["menu_right"].depth, 1.0f);
    ensure_equals("P1 is unaffected", input.GetController(0)->key_down["look_left"].depth, 0.0f);
    config.GetRef("gamepad_11_invert_x_look") = false;
    config.GetRef("gamepad_11_invert_y_look") = true;
    input.ProcessController(11, 0.01f);
    ensure_equals("X returns to normal", input.GetController(11)->key_down["look_right"].depth, 1.5f);
    ensure_equals("old inverted X released", input.GetController(11)->key_down["look_left"].depth, 0.0f);
    ensure_equals("Y inverted once", input.GetController(11)->key_down["look_up"].depth, 1.0f);
}
template <>
template <>
void localinput::test<7>() {
    set_test_name("an already-ungrabbed pointer restores a cursor hidden during loading");
    GameCursor cursor;
    input.cursor = &cursor;
    ensure("pointer already free", !input.GetGrabMouse());
    cursor.SetVisible(false);
    input.SetGrabMouse(false);
    ensure("pause/menu ungrab restores software cursor", cursor.visible);
    cursor.SetVisible(false);
    input.SetGrabMouse(false);
    ensure("repeated menu request restores visibility again", cursor.visible);
    input.cursor = nullptr;
    input.SetGrabMouse(false);
}
}  // namespace tut
