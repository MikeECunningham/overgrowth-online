#include <UserInput/input.h>
#include <Internal/config.h>
#include <SDL.h>
#include <UserInput/keyTranslator.h>
#include <tut/tut.hpp>

namespace tut {
struct datalocalmapping {
    Config saved = config;
    Input input;
    ~datalocalmapping() { config = saved; }
};
typedef test_group<datalocalmapping> tg_localmapping;
tg_localmapping test_group_localmapping("LocalInputMapping");
typedef tg_localmapping::object localmapping;

template <> template <>
void localmapping::test<1>() {
    set_test_name("an extra local player cannot replace an online remote input slot");
    input.SetUpForXPlayers(4, true);
    const int remote = input.AllocateRemotePlayerInput();
    input.GetController(remote)->key_down["up"].depth = 0.75f;
    input.SetUpForXPlayers(5, true);
    const int fifth = input.GetLocalPlayerInputID(4);
    ensure("local and remote inputs are distinct", fifth != remote);
    ensure("remote keeps ownership", input.GetController(remote)->remote_controlled);
    ensure_equals("remote input is retained", input.GetController(remote)->key_down["up"].depth, 0.75f);
    ensure("fifth local input enabled", input.GetController(fifth)->enabled && !input.GetController(fifth)->remote_controlled);
    ensure_equals("actual input maps back to logical profile", input.LocalPlayerIndexForInputID(fifth), 4);
    input.SetUpForXPlayers(5, true);
    ensure_equals("mapping remains stable", input.GetLocalPlayerInputID(4), fifth);
}

template <> template <>
void localmapping::test<2>() {
    set_test_name("controller settings follow logical player rather than physical input ID");
    config.GetRef("gamepad_4_look_sensitivity") = 1.75f;
    config.GetRef("gamepad_4_deadzone") = 0.12f;
    input.SetUpForXPlayers(4, true);
    input.AllocateRemotePlayerInput();
    input.SetUpForXPlayers(5, true);
    auto& pad = input.GetOpenJoysticks()[10];
    input.BindLocalJoystick(10, 4);
    ensure_equals("pad uses mapped physical input", pad->player_input, input.GetLocalPlayerInputID(4));
    ensure_equals("sensitivity uses local profile four", pad->joystick.look_sensitivity_, 1.75f);
    ensure_equals("deadzone uses local profile four", pad->joystick.deadzone, 0.12f);
}
template <> template <>
void localmapping::test<3>() {
    set_test_name("early or empty controller profiles do not disable camera movement");
    config.map_.clear();
    input.SetUpForXPlayers(2);
    auto& pad = input.GetOpenJoysticks()[10];
    input.BindLocalJoystick(10, 1);
    ensure("usable sensitivity before defaults load", pad->joystick.look_sensitivity_ > 0.0f);
    config.GetRef("gamepad_look_sensitivity") = 1.25f;
    config.GetRef("gamepad_deadzone") = 0.075f;
    config.GetRef("gamepad_1_look_sensitivity") = "";
    input.SetFromConfig(config);
    ensure_equals("late defaults replace empty profile", pad->joystick.look_sensitivity_, 1.25f);
    config.GetRef("gamepad_1_look_sensitivity") = 0.0f;
    input.SetFromConfig(config);
    ensure_equals("old invalid zero profile is repaired", pad->joystick.look_sensitivity_, 1.25f);
    config.GetRef("gamepad_1_deadzone") = 0.0f;
    input.UpdateGamepadDeadzone();
    ensure_equals("intentional zero deadzone is retained", pad->joystick.deadzone, 0.0f);
}
template <> template <>
void localmapping::test<4>() {
    set_test_name("controller disconnect keeps online input ownership above four locals");
    input.SetUpForXPlayers(4, true);
    const int remote = input.AllocateRemotePlayerInput();
    input.SetUpForXPlayers(5, true);
    const int fifth = input.GetLocalPlayerInputID(4);
    auto& pad = input.GetOpenJoysticks()[10];
    pad->player_input = fifth;
    pad->sdl_joystick = nullptr;
    input.GetController(remote)->key_down["up"].depth = 0.75f;
    SDL_Event event = {};
    event.type = SDL_JOYDEVICEREMOVED;
    event.jdevice.which = 10;
    input.HandleEvent(event);
    ensure_equals("local input mapping survives hotplug", input.GetLocalPlayerInputID(4), fifth);
    ensure("remote is still remote", input.GetController(remote)->remote_controlled);
    ensure_equals("remote input remains intact", input.GetController(remote)->key_down["up"].depth, 0.75f);
}
template <> template <>
void localmapping::test<5>() {
    set_test_name("online controller claims stay with the lobby while offline menus share P1");
    auto& first = input.GetOpenJoysticks()[10];
    first->player_input = -1;
    input.SetUpForXPlayers(1, true);
    ensure_equals("unknown online pad stays unassigned", first->player_input, -1);
    input.BindLocalJoystick(10, 0);
    auto& second = input.GetOpenJoysticks()[20];
    second->player_input = -1;
    input.SetUpForXPlayers(2, true);
    ensure_equals("growing roster retains P1 device", first->player_input, 0);
    ensure_equals("new device waits for a lobby claim", second->player_input, -1);
    SDL_Event event = {};
    event.type = SDL_JOYBUTTONDOWN;
    event.jbutton.which = 20;
    event.jbutton.timestamp = 123;
    input.HandleEvent(event);
    event.type = SDL_JOYBUTTONUP;
    event.jbutton.timestamp = 124;
    input.HandleEvent(event);
    ensure_equals("last press records the exact device", input.LastControllerButtonInstance(), 20);
    ensure_equals("release does not replace the press time", input.LastControllerButtonTime(), 123.0f);
    input.SetUpForXPlayers(0);
    ensure_equals("offline menu accepts first pad", first->player_input, 0);
    ensure_equals("offline menu accepts second pad", second->player_input, 0);
}
template <> template <>
void localmapping::test<6>() {
    set_test_name("removing a local seat preserves surviving devices, profiles and remote slots");
    config.GetRef("gamepad_4_look_sensitivity") = 1.9f;
    config.GetRef("gamepad_4_invert_x_look") = true;
    config.GetRef("gamepad_4[jump]") = "b";
    input.SetFromConfig(config);
    input.SetUpForXPlayers(4, true);
    const int remote = input.AllocateRemotePlayerInput();
    input.SetUpForXPlayers(5, true);
    const int last = input.GetLocalPlayerInputID(4);
    input.GetOpenJoysticks()[10]->player_input = last;
    input.BindLocalKeyboard(4);
    input.RemoveLocalPlayerInput(1);
    ensure_equals("surviving input ID stays stable", input.GetLocalPlayerInputID(3), last);
    ensure_equals("surviving device keeps its actor", input.GetOpenJoysticks()[10]->player_input, last);
    ensure_equals("keyboard keeps its actor", input.LocalKeyboardPlayerIndex(), 3);
    ensure("remote remains remote", input.GetController(remote)->remote_controlled);
    ensure_equals("sensitivity follows surviving player", config["gamepad_3_look_sensitivity"].toNumber<float>(), 1.9f);
    ensure("inversion follows surviving player", config["gamepad_3_invert_x_look"].toBool());
    ensure_equals("binding follows surviving player", input.GetBindingValue("gamepad_3", "jump"), std::string("b"));
    input.RemoveLocalPlayerInput(0);
    ensure_equals("former primary can be removed after role transfer", input.GetNumLocalPlayers(), 3u);
    ensure_equals("physical input survives both removals", input.GetLocalPlayerInputID(2), last);
    ensure_equals("profile survives both removals", config["gamepad_2_look_sensitivity"].toNumber<float>(), 1.9f);
}
template <> template <>
void localmapping::test<7>() {
    set_test_name("keyboard gameplay joins a distinct player while menus remain navigable");
    InitKeyTranslator();
    config.GetRef("key[up]") = "w";
    config.GetRef("key[menu_return]") = "return";
    input.SetFromConfig(config);
    input.SetUpForXPlayers(4, true);
    input.AllocateRemotePlayerInput();
    input.SetUpForXPlayers(5, true);
    const int keyboard_id = input.GetLocalPlayerInputID(4);
    input.BindLocalKeyboard(4);
    auto& pad = input.GetOpenJoysticks()[10];
    pad->player_input = 0;
    pad->joystick.buttons_down_["up"] = 0.4f;
    SDL_Keysym key = {};
    key.sym = SDLK_w; key.scancode = SDL_SCANCODE_W;
    input.getKeyboard().handleKeyDown(key);
    input.ProcessController(0, 0.01f);
    input.ProcessController(keyboard_id, 0.01f);
    ensure_equals("P1 only receives its pad", input.GetController(0)->key_down["up"].depth, 0.4f);
    ensure_equals("joined keyboard controls its own actor", input.GetController(keyboard_id)->key_down["up"].depth, 1.0f);
    SDL_Event event = {};
    event.type = SDL_KEYDOWN; event.key.keysym.sym = SDLK_RETURN;
    input.RecordKeyboardJoinEvent(event);
    ensure("Enter produces a join request", input.KeyboardJoinPressed());
    ensure("join press is consumed once", !input.KeyboardJoinPressed());
    event.key.repeat = 1;
    input.RecordKeyboardJoinEvent(event);
    ensure("key repeat cannot add players", !input.KeyboardJoinPressed());
}
template <> template <>
void localmapping::test<8>() {
    set_test_name("a joined keyboard navigates only its own menu during play");
    InitKeyTranslator();
    config.GetRef("key[menu_return]") = "return";
    input.SetFromConfig(config);
    input.SetUpForXPlayers(2, true);
    input.BindLocalKeyboard(1);
    input.SetKeyboardMenuNavigationShared(false);
    SDL_Keysym key = {};
    key.sym = SDLK_RETURN; key.scancode = SDL_SCANCODE_RETURN;
    input.getKeyboard().handleKeyDown(key);
    input.ProcessController(0, 0.01f);
    input.ProcessController(1, 0.01f);
    ensure_equals("P1 does not receive another player's accept", input.GetController(0)->key_down["menu_return"].depth, 0.0f);
    ensure_equals("keyboard owner's accept is retained", input.GetController(1)->key_down["menu_return"].depth, 1.0f);
    input.SetKeyboardMenuNavigationShared(true);
    input.ProcessController(0, 0.01f);
    ensure_equals("pregame can still navigate P1 with the keyboard", input.GetController(0)->key_down["menu_return"].depth, 1.0f);
}
template <> template <>
void localmapping::test<9>() {
    set_test_name("removing former keyboard primary preserves the promoted pad's identity and settings");
    config.GetRef("gamepad_1_look_sensitivity") = 1.7f;
    config.GetRef("gamepad_1_invert_y_look") = true;
    config.GetRef("gamepad_1[jump]") = "b";
    input.SetFromConfig(config);
    input.SetUpForXPlayers(2, true);
    const int survivor = input.GetLocalPlayerInputID(1);
    auto& pad = input.GetOpenJoysticks()[10];
    input.BindLocalJoystick(10, 1);
    input.BindLocalKeyboard(0);
    config.GetRef("controller_menu_owner") = 1;
    config.GetRef("menu_player_config") = 1;
    input.RemoveLocalPlayerInput(0);
    ensure_equals("remaining player keeps actual input", input.GetLocalPlayerInputID(0), survivor);
    ensure_equals("remaining pad keeps assignment", pad->player_input, survivor);
    ensure_equals("removed keyboard is unassigned", input.LocalKeyboardPlayerIndex(), -1);
    ensure_equals("shared menu owner follows surviving logical seat", config["controller_menu_owner"].toNumber<int>(), 0);
    ensure_equals("profile editor follows surviving logical seat", config["menu_player_config"].toNumber<int>(), 0);
    ensure_equals("pad sensitivity follows its profile", pad->joystick.look_sensitivity_, 1.7f);
    ensure("inversion follows its profile", config["gamepad_0_invert_y_look"].toBool());
    ensure_equals("bindings follow its profile", input.GetBindingValue("gamepad_0", "jump"), std::string("b"));
    input.RemoveLocalPlayerInput(0);
    ensure_equals("last local player cannot be removed", input.GetNumLocalPlayers(), 1u);
}
}  // namespace tut
