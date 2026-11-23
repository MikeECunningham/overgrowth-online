//-----------------------------------------------------------------------------
//           Name: testmain.h
//      Developer: Wolfire Games LLC
//    Modified By: Overgrowth Online (crxhvrd)
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
#include "input.h"

#include <Graphics/graphics.h>
#include <Graphics/Cursor.h>

#include <UserInput/keyTranslator.h>
#include <UserInput/keycommands.h>

#include <Internal/config.h>
#include <Internal/timer.h>
#include <Internal/profiler.h>

#include <GUI/gui.h>
#include <Internal/file_descriptor.h>
#include <Logging/logdata.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <cctype>

extern Timer ui_timer;

static void CompleteGamepadBindings(const std::string& type, BindMap& bind_map);

Input::JoystickStruct::JoystickStruct()
    : joystick(1.0f) {
}

namespace {
bool IsPlayerGamepadCategory(const std::string& category) {
    if (category.size() <= 8 || category.compare(0, 8, "gamepad_") != 0) {
        return false;
    }
    return std::all_of(category.begin() + 8, category.end(), [](unsigned char c) { return std::isdigit(c); });
}

void StringFromBind(std::string* str, const SDL_GameControllerButtonBind& bind) {
    std::ostringstream oss;
    switch (bind.bindType) {
        case SDL_CONTROLLER_BINDTYPE_NONE:
            oss << "none";
            break;
        case SDL_CONTROLLER_BINDTYPE_BUTTON:
            oss << "button" << bind.value.button + 1;
            break;
        case SDL_CONTROLLER_BINDTYPE_AXIS:
            oss << "axis" << bind.value.axis + 1;
            break;
        case SDL_CONTROLLER_BINDTYPE_HAT:
            oss << "hat" << bind.value.hat.hat << " " << bind.value.hat.hat_mask;
            break;
    }
    *str = oss.str();
}

void PrintControllers(const Input::JoystickMap& open_joysticks) {
    LOGI << "Current controllers:" << std::endl;
    for (const auto& open_joystick : open_joysticks) {
        const Input::RC_JoystickStruct& js = open_joystick.second;
        SDL_Joystick* joystick = (SDL_Joystick*)js.GetConst().sdl_joystick;
        LOGI << "Joystick" << open_joystick.first << "," << SDL_JoystickName(joystick) << std::endl;
    }
}
}  // namespace

Input::Input() : ignore_mouse_frame(false),
                 num_players_(0),
                 quit_requested_(false),
                 in_focus_(true),
                 grab_mouse_(false),
                 invert_x_mouse_look_(false),
                 invert_y_mouse_look_(false),
                 use_raw_input(false),
                 last_controller_event_time(0.0f),
                 last_mouse_event_time(0.0f),
                 last_keyboard_event_time(0.0f),
                 joystick_sequence_id(0),
                 allow_controller_input_(true),
                 use_controller_input_(false),
                 joystick_assignment_dirty_(true) {
    player_inputs_.resize(4);

    // Default to only having one active player input.
    player_inputs_[0].enabled = true;
    player_inputs_[1].enabled = false;
    player_inputs_[2].enabled = false;
    player_inputs_[3].enabled = false;
}

void Input::SetInvertXMouseLook(bool val) {
    invert_x_mouse_look_ = val;
}

void Input::SetInvertYMouseLook(bool val) {
    invert_y_mouse_look_ = val;
}

void Input::UpdateGamepadLookSensitivity() {
    for (auto& open_joystick : open_joysticks_) {
        const int profile = LocalPlayerIndexForInputID(open_joystick.second->player_input);
        if (profile < 0) {
            continue;
        }
        char buffer[64];
        sprintf(buffer, "gamepad_%i_look_sensitivity", profile);
        float sensitivity = config[buffer].toNumber<float>();
        if (sensitivity <= 0.0f) sensitivity = config["gamepad_look_sensitivity"].toNumber<float>();
        open_joystick.second->joystick.look_sensitivity_ = sensitivity > 0.0f ? sensitivity : 1.25f;
    }
}

void Input::UpdateGamepadDeadzone() {
    for (auto& open_joystick : open_joysticks_) {
        const int profile = LocalPlayerIndexForInputID(open_joystick.second->player_input);
        if (profile < 0) {
            continue;
        }
        char buffer[64];
        sprintf(buffer, "gamepad_%i_deadzone", profile);
        const std::string key = config[buffer].raw().empty() ? "gamepad_deadzone" : buffer;
        open_joystick.second->joystick.deadzone = config[key].raw().empty() ? 0.075f : config[key].toNumber<float>();
    }
}

void Input::OpenJoystick(int index) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    // Opt-in process-local test isolation: real pads may still be used by a
    // person in another window while our test's SDL virtual pads join/play.
    const char* virtual_only = SDL_getenv("OGMP_VIRTUAL_CONTROLLERS_ONLY");
    if (virtual_only && SDL_strcmp(virtual_only, "1") == 0 && !SDL_JoystickIsVirtual(index)) {
        LOGI << "Test input: ignoring physical joystick " << index << std::endl;
        return;
    }
#endif
    // int num_joysticks = SDL_NumJoysticks();

    // If joystick has a GameController definition, extract mapping
    SDL_GameControllerButtonBind null_bind;
    null_bind.bindType = SDL_CONTROLLER_BINDTYPE_NONE;
    null_bind.value.axis = 0;
    std::vector<SDL_GameControllerButtonBind> gamepad_binding;

    if (SDL_IsGameController(index)) {
        const char* name = SDL_GameControllerNameForIndex(index);
        LOGI << "Attached controller " << index << ":" << (name ? name : "Unknown Controller") << std::endl;
        SDL_GameController* controller = SDL_GameControllerOpen(index);

        if (!controller) {
            LOGW << "Could not open GameController: " << SDL_GetError() << std::endl;
            return;
        }

        gamepad_binding.resize(ControllerInput::NUM_INPUTS, null_bind);

        for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i) {
            SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(controller, (SDL_GameControllerAxis)i);
            ControllerInput::Input input = SDLControllerAxisToController((SDL_GameControllerAxis)i);
            if (input != ControllerInput::NONE) {
                gamepad_binding[input] = bind;
            }
        }

        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
            SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(controller, (SDL_GameControllerButton)i);
            ControllerInput::Input input = SDLControllerButtonToController((SDL_GameControllerButton)i);
            if (input != ControllerInput::NONE) {
                gamepad_binding[input] = bind;
            }
        }

        SDL_GameControllerClose(controller);
    } else {
        return;  // Only handle joysticks that have a GameController entry for now
    }

    const char* name = SDL_JoystickNameForIndex(index);
    LOGI << "Attached joystick " << index << ":" << (name ? name : "Unknown Joystick") << std::endl;
    SDL_Joystick* joystick = SDL_JoystickOpen(index);

    if (!joystick) {
        DisplayError("Error", "Could not open Joystick");
        return;
    }

    // Add configured bindings to binding internal map
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    RC_JoystickStruct& js = open_joysticks_[id];
    js->sdl_joystick = joystick;
    js->gamepad_bind = gamepad_binding;
    // char buffer[64];
    // sprintf(buffer, "gamepad%d_deadzone", id);
    js->player_input = -1;  // Assigned to a local slot by SetUpForXPlayers.
    joystick_assignment_dirty_ = true;
    js->joystick.deadzone = config["gamepad_0_deadzone"].toNumber<float>();

    /*SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(joystick);
    SDL_Haptic* haptic2 = SDL_HapticOpen(0);
    js.sdl_haptic = haptic;
    if(js.sdl_haptic){
        if (SDL_HapticRumbleInit(haptic) == 0) {
            if (SDL_HapticRumblePlay(haptic, 0.5, 2000) == 0) {
            }
        }
    }*/
}

void Input::CloseJoystick(int instance_id) {
    JoystickMap::iterator iter = open_joysticks_.find(instance_id);
    if (iter != open_joysticks_.end()) {
        RC_JoystickStruct js = iter->second;
        SDL_Joystick* joystick = (SDL_Joystick*)js->sdl_joystick;
        SDL_JoystickClose(joystick);
        open_joysticks_.erase(iter);
        joystick_assignment_dirty_ = true;
    }
}

static void AddGameControllerDB() {
    // Load mappings from crowd-sourced controller database
    // Update at:
    // https://github.com/gabomdq/SDL_GameControllerDB/blob/master/gamecontrollerdb.txt
    DiskFileDescriptor file;
    std::vector<char> file_buf;
    if (!file.Open("Data/gamecontrollerdb.txt", "r")) {
        return;
    }
    int file_size = file.GetSize();
    file_buf.resize(file_size);

    int bytes_read = file.ReadBytesPartial(&file_buf[0], file_size);
    file_buf.resize(bytes_read + 1);
    file_buf.back() = '\0';
    file.Close();

    SDL_RWops* SDL_file = SDL_RWFromConstMem(&file_buf[0], file_buf.size());
    if (SDL_file) {
        int mappings = SDL_GameControllerAddMappingsFromRW(SDL_file, 1);
        LOGI << "Added " << mappings << " controller mappings." << std::endl;
    }
}

void Input::Initialize() {
    InitKeyTranslator();
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        FatalError("Error", "Could not initialize SDL_INIT_GAMECONTROLLER");
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
        FatalError("Error", "Could not initialize SDL_INIT_JOYSTICK");
    }
    // if (SDL_InitSubSystem(SDL_INIT_HAPTIC) < 0) {
    //     FatalError("Error", "Could not initialize SDL_INIT_HAPTIC");
    // }
    AddGameControllerDB();
    SDL_JoystickEventState(SDL_ENABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        OpenJoystick(i);
    }
    PrintControllers(open_joysticks_);
    ProcessBindings();
    SetUpForXPlayers(0);
}

void Input::Dispose() {
    for (auto& open_joystick : open_joysticks_) {
        RC_JoystickStruct js = open_joystick.second;
        SDL_Joystick* joystick = (SDL_Joystick*)js->sdl_joystick;
        SDL_JoystickClose(joystick);
    }

    // SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

bool Input::GetGrabMouse() {
    return grab_mouse_;
}

void UIShowCursor(bool show) {
    if (show) {
        SDL_ShowCursor(SDL_ENABLE);
    } else {
        SDL_ShowCursor(SDL_DISABLE);
    }
}

bool UICursorVisible() {
    int v = SDL_ShowCursor(SDL_QUERY);
    if (v == SDL_ENABLE) {
        return true;
    } else if (v == SDL_ENABLE) {
        return false;
    } else {
        LOGE << "Mouse show error" << std::endl;
        return false;
    }
}

void Input::SetGrabMouse(bool grab) {
    if (grab_mouse_ == grab) {
        // Loading/cutscenes may hide the software cursor while the pointer is
        // already free. A pause menu still needs to restore its visibility.
        if (!grab && cursor != nullptr) {
            cursor->SetVisible(true);
        }
        return;
    }
    if (grab && in_focus_) {
        grab_mouse_ = true;
        Graphics::Instance()->SetWindowGrab(true);
        if (cursor != nullptr) cursor->SetVisible(false);
        if (use_raw_input) {
            // If we further wish to do this "correctly" we can use this hint: https://wiki.libsdl.org/SDL_HINT_MOUSE_RELATIVE_MODE_WARP
            SDL_SetRelativeMouseMode(SDL_TRUE);
        } else {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    } else {
        grab_mouse_ = false;
        Graphics::Instance()->SetWindowGrab(false);
        if (cursor != nullptr) cursor->SetVisible(true);
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }
}

void Input::HandleEvent(const SDL_Event& event) {
    PROFILER_ZONE(g_profiler_ctx, "Input::HandleEvent");
    RecordKeyboardJoinEvent(event);
    // Redirect input events to the controller
    switch (event.type) {
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    in_focus_ = false;
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    if (grab_mouse_) {
                        Graphics::Instance()->SetWindowGrab(true);
                        cursor->SetVisible(false);
                    }
                    in_focus_ = true;
                    break;
            }
            break;
        case SDL_MOUSEMOTION:
            if (in_focus_) {
                last_mouse_event_time = (float)event.common.timestamp;
            }
            if (use_raw_input) {
                mouse_.delta_[0] += event.motion.xrel;
                mouse_.delta_[1] += event.motion.yrel;
                mouse_.pos_[0] = event.motion.x;
                mouse_.pos_[1] = event.motion.y;
            } else {
                int width;
                int height;
                SDL_GetWindowSize(Graphics::Instance()->sdl_window_, &width, &height);
                if (!grab_mouse_ || event.motion.x != width / 2 || event.motion.y != height / 2) {
                    mouse_.delta_[0] += event.motion.xrel;
                    mouse_.delta_[1] += event.motion.yrel;
                    mouse_.pos_[0] = event.motion.x;
                    mouse_.pos_[1] = event.motion.y;
                }
                if (grab_mouse_ && in_focus_ && (event.motion.x < 100 || event.motion.y < 100 || event.motion.x > width - 100 || event.motion.y > height - 100)) {
                    PROFILER_ZONE(g_profiler_ctx, "WarpMouse");
                    SDL_WarpMouseInWindow(Graphics::Instance()->sdl_window_, width / 2, height / 2);
                }
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (in_focus_) {
                last_mouse_event_time = (float)event.common.timestamp;
            }
            if (!ignore_mouse_frame) {
                HandleMouseButtonDown(event.button.button, event.button.clicks);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (in_focus_) {
                last_mouse_event_time = (float)event.common.timestamp;
            }
            if (!ignore_mouse_frame) {
                mouse_.MouseWheelEvent(event.wheel.x, event.wheel.y);
            }
            break;
        case SDL_MOUSEBUTTONUP: {
            if (in_focus_) {
                last_mouse_event_time = (float)event.common.timestamp;
            }
            if (!ignore_mouse_frame) {
                HandleMouseButtonUp(event.button.button);
            }
            break;
        }
        case SDL_KEYDOWN: {
            if (in_focus_) {
                last_keyboard_event_time = (float)event.common.timestamp;
            }
            const SDL_Keysym& keysym = event.key.keysym;
            const SDL_Scancode& scancode = keysym.scancode;
            const SDL_Keycode& sdl_key = keysym.sym;

            if (scancode == StringToSDLScancode(bindings_["key"]["rclick"])) {
                HandleMouseButtonDown(3, 1);
                break;
            }

            if (event.key.repeat == false) {
                keyboard_.handleKeyDownFirst(keysym);
            }
            keyboard_.handleKeyDown(keysym);
            KeyCommand::HandleKeyDownEvent(keyboard_, keysym);
            // printf("Keydown: %s\n", SDLKeyToString(sdl_key));
            break;
        }
        case SDL_KEYUP: {
            if (in_focus_) {
                last_keyboard_event_time = (float)event.common.timestamp;
            }
            const SDL_Keysym& keysym = event.key.keysym;
            const SDL_Scancode& scancode = keysym.scancode;
            const SDL_Keycode& sdl_key = keysym.sym;
            if (scancode == StringToSDLScancode(bindings_["key"]["rclick"])) {
                HandleMouseButtonUp(3);
                break;
            }
            keyboard_.handleKeyUp(keysym);
            break;
        }
        case SDL_JOYAXISMOTION: {
            if (in_focus_) {
                last_controller_event_time = (float)event.common.timestamp;
            }
            SDL_JoyAxisEvent* sdl_joy = (SDL_JoyAxisEvent*)&event.jaxis;
            JoystickMap::iterator iter = open_joysticks_.find(sdl_joy->which);
            if (iter != open_joysticks_.end()) {
                const std::vector<SDL_GameControllerButtonBind>& gamepad_bind = iter->second->gamepad_bind;
                // TODO: Replace for-loop with constant-time lookup?
                for (size_t i = 0; i < gamepad_bind.size(); ++i) {
                    if (gamepad_bind[i].bindType == SDL_CONTROLLER_BINDTYPE_AXIS && gamepad_bind[i].value.axis == sdl_joy->axis) {
                        ControllerInput::Input input = (ControllerInput::Input)i;
                        ControllerInput::Input opposite_input = ControllerInput::NONE;
                        switch (input) {
                            case ControllerInput::L_STICK_X:
                                input = (sdl_joy->value >= 0.0f ? ControllerInput::L_STICK_XP : ControllerInput::L_STICK_XN);
                                opposite_input = (sdl_joy->value < 0.0f ? ControllerInput::L_STICK_XP : ControllerInput::L_STICK_XN);
                                break;
                            case ControllerInput::L_STICK_Y:
                                input = (sdl_joy->value >= 0.0f ? ControllerInput::L_STICK_YP : ControllerInput::L_STICK_YN);
                                opposite_input = (sdl_joy->value < 0.0f ? ControllerInput::L_STICK_YP : ControllerInput::L_STICK_YN);
                                break;
                            case ControllerInput::R_STICK_X:
                                input = (sdl_joy->value >= 0.0f ? ControllerInput::R_STICK_XP : ControllerInput::R_STICK_XN);
                                opposite_input = (sdl_joy->value < 0.0f ? ControllerInput::R_STICK_XP : ControllerInput::R_STICK_XN);
                                break;
                            case ControllerInput::R_STICK_Y:
                                input = (sdl_joy->value >= 0.0f ? ControllerInput::R_STICK_YP : ControllerInput::R_STICK_YN);
                                opposite_input = (sdl_joy->value < 0.0f ? ControllerInput::R_STICK_YP : ControllerInput::R_STICK_YN);
                                break;
                            case ControllerInput::L_TRIGGER:
                            case ControllerInput::R_TRIGGER:
                                // Remap triggers from {-X, X} to { 0, X }
                                sdl_joy->value = ((sdl_joy->value + 32767) / 2);
                                break;
                            default:
                                break;
                        }
                        joystick_sequence_id += iter->second->joystick.HandleInputChange(input, sdl_joy->value, joystick_sequence_id);
                        if (opposite_input != ControllerInput::NONE) {
                            joystick_sequence_id += iter->second->joystick.HandleInputChange(opposite_input, 0.0f, joystick_sequence_id);
                        }
                        break;
                    }
                }
            }
            break;
        }
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP: {
            if (in_focus_) {
                last_controller_event_time = (float)event.common.timestamp;
            }
            SDL_JoyButtonEvent* button = (SDL_JoyButtonEvent*)&event.jbutton;
            JoystickMap::iterator iter = open_joysticks_.find(button->which);
            if (iter != open_joysticks_.end()) {
                if (in_focus_ && event.type == SDL_JOYBUTTONDOWN) {
                    last_controller_button_instance_ = button->which;
                    last_controller_button_time_ = static_cast<float>(event.common.timestamp);
                }
                // TODO: Replace for-loop with constant-time lookup?
                const std::vector<SDL_GameControllerButtonBind>& gamepad_bind = iter->second->gamepad_bind;
                for (size_t i = 0; i < gamepad_bind.size(); ++i) {
                    if (gamepad_bind[i].bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && gamepad_bind[i].value.button == button->button) {
                        joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, (button->state == SDL_PRESSED), joystick_sequence_id);
                        break;
                    }
                }
            }
            break;
        }
        case SDL_JOYHATMOTION: {
            if (in_focus_) {
                last_controller_event_time = (float)event.common.timestamp;
            }
            SDL_JoyHatEvent* hat = (SDL_JoyHatEvent*)&event.jhat;
            JoystickMap::iterator iter = open_joysticks_.find(hat->which);
            if (iter != open_joysticks_.end()) {
                // TODO: Replace for-loop with constant-time lookup?
                const std::vector<SDL_GameControllerButtonBind>& gamepad_bind = iter->second->gamepad_bind;
                for (size_t i = 0; i < gamepad_bind.size(); ++i) {
                    if (gamepad_bind[i].bindType == SDL_CONTROLLER_BINDTYPE_HAT && gamepad_bind[i].value.hat.hat == hat->hat) {
                        switch (hat->value) {
                            case SDL_HAT_LEFTUP:
                                if (gamepad_bind[i].value.hat.hat_mask == SDL_HAT_LEFT ||
                                    gamepad_bind[i].value.hat.hat_mask == SDL_HAT_UP) {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 1.0f, joystick_sequence_id);
                                } else {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 0.0f, joystick_sequence_id);
                                }
                                break;
                            case SDL_HAT_RIGHTUP:
                                if (gamepad_bind[i].value.hat.hat_mask == SDL_HAT_RIGHT ||
                                    gamepad_bind[i].value.hat.hat_mask == SDL_HAT_UP) {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 1.0f, joystick_sequence_id);
                                } else {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 0.0f, joystick_sequence_id);
                                }
                                break;
                            case SDL_HAT_LEFTDOWN:
                                if (gamepad_bind[i].value.hat.hat_mask == SDL_HAT_LEFT ||
                                    gamepad_bind[i].value.hat.hat_mask == SDL_HAT_DOWN) {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 1.0f, joystick_sequence_id);
                                } else {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 0.0f, joystick_sequence_id);
                                }
                                break;
                            case SDL_HAT_RIGHTDOWN:
                                if (gamepad_bind[i].value.hat.hat_mask == SDL_HAT_RIGHT ||
                                    gamepad_bind[i].value.hat.hat_mask == SDL_HAT_DOWN) {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 1.0f, joystick_sequence_id);
                                } else {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 0.0f, joystick_sequence_id);
                                }
                                break;
                            default:
                                if (gamepad_bind[i].value.hat.hat_mask == hat->value) {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 1.0f, joystick_sequence_id);
                                } else {
                                    joystick_sequence_id += iter->second->joystick.HandleInputChange((ControllerInput::Input)i, 0.0f, joystick_sequence_id);
                                }
                                break;
                        }
                    }
                }
            }
            break;
        }
        case SDL_JOYDEVICEADDED:
            LOGI << "Added device:" << event.jdevice.which << std::endl;
            OpenJoystick(event.jdevice.which);
            PrintControllers(open_joysticks_);
            SetUpForXPlayers(num_players_, preserve_remote_inputs_);
            ProcessBindings();
            break;
        case SDL_JOYDEVICEREMOVED:
            LOGI << "Removed device:" << event.jdevice.which << std::endl;
            CloseJoystick(event.jdevice.which);
            PrintControllers(open_joysticks_);
            SetUpForXPlayers(num_players_, preserve_remote_inputs_);
            break;
    }
}

namespace {
void PlayerInputKeyDown(KeyState* keyState) {
    if (keyState->count <= 0) {
        keyState->count *= -1;
        keyState->count++;
        keyState->depth_count++;
        keyState->depth = 1.0f;
    }
}
}  // namespace

std::map<std::string, Input::InjectedKey>& Input::InjectedKeysForController(int controller_id) {
    return controller_id == 0 ? injected_keys_ : injected_player_keys_[controller_id];
}

float* Input::InjectedLookForController(int controller_id) {
    return controller_id == 0 ? injected_look_ : injected_player_look_[controller_id].data();
}

void Input::ApplyInjectedInput(PlayerInput& control, int controller_id) {
    auto keys_it = injected_player_keys_.find(controller_id);
    auto* keys = controller_id == 0 ? &injected_keys_ :
                 keys_it != injected_player_keys_.end() ? &keys_it->second : NULL;
    if (keys) {
        for (auto it = keys->begin(); it != keys->end();) {
            KeyState& ks = control.key_down[it->first];

            if (it->second.depth >= KeyState::kDepthThreshold) {
                PlayerInputKeyDown(&ks);  // the keyboard's own semantics
                ks.depth = it->second.depth;
            } else if (it->second.depth > 0.0f) {
                if (ks.count <= 0) {  // analog, like the joystick path
                    ks.count *= -1;
                    ++ks.count;
                }
                ks.depth = it->second.depth;
            }

            if (it->second.steps_left > 0 && --it->second.steps_left == 0) {
                it = keys->erase(it);
            } else {
                ++it;
            }
        }
    }

    auto look_it = injected_player_look_.find(controller_id);
    float* look = controller_id == 0 ? injected_look_ :
                  look_it != injected_player_look_.end() ? look_it->second.data() : NULL;
    if (look && (look[0] != 0.0f || look[1] != 0.0f)) {
        control.key_down["look_right"].depth += look[0];
        control.key_down["look_down"].depth += look[1];
        look[0] = 0.0f;
        look[1] = 0.0f;
    }
}

void Input::ProcessController(int controller_id, float timestep) {
    if (controller_id < 0 || static_cast<size_t>(controller_id) >= player_inputs_.size()) {
        return;
    }
    PlayerInput& control = player_inputs_[controller_id];

    // Early out if we're remote controlled via network.
    if (control.remote_controlled) {
        return;
    }

    // Early out if we're disabled.
    if (control.enabled == false) {
        return;
    }

    // Set all keydown counts to negative
    PlayerInput::KeyDownMap& kd = control.key_down;
    for (auto& iter : kd) {
        iter.second.count *= -1;
        iter.second.depth = 0.0f;
    }
    std::set<std::string> active_buttons;
    // If any bound controller button is pressed more than halfway in
    // (disregarding sensitivity settings), controller input is accepted until
    // any mouse/keyboard bind is pressed.
    // This is to avoid misconfigured deadzone settings to spook around
    if (!use_controller_input_) {
        bool run = true;
        for (JoystickMap::iterator iter = open_joysticks_.begin(); run && iter != open_joysticks_.end(); ++iter) {
            const RC_JoystickStruct js = iter->second;
            if (js.GetConst().player_input != controller_id) {
                continue;
            }
            const Joystick& joystick = js.GetConst().joystick;
            const Joystick::ButtonMap& bm = joystick.buttons_down_;
            for (const auto& iter2 : bm) {
                if (iter2.second) {
                    float depth = iter2.second;
                    if (depth > 0.5f) {
                        use_controller_input_ = true;
                        run = false;
                        break;
                    }
                }
            }
        }
    }
    // Joystick input
    if (allow_controller_input_ && (use_controller_input_ || num_players_ > 1)) {
        const int local_index = std::max(0, LocalPlayerIndexForInputID(controller_id));
        const std::string profile = "gamepad_" + std::to_string(local_index);
        const bool invert_x = config[profile + "_invert_x_look"].toNumber<bool>();
        const bool invert_y = config[profile + "_invert_y_look"].toNumber<bool>();
        for (auto& open_joystick : open_joysticks_) {
            const RC_JoystickStruct js = open_joystick.second;
            if (js.GetConst().player_input != controller_id) {
                continue;
            }
            const Joystick& joystick = js.GetConst().joystick;
            const Joystick::ButtonMap& bm = joystick.buttons_down_;
            for (const auto& iter2 : bm) {
                if (iter2.second) {
                    std::string binding = iter2.first;
                    if (invert_x) {
                        if (binding == "look_left") binding = "look_right";
                        else if (binding == "look_right") binding = "look_left";
                    }
                    if (invert_y) {
                        if (binding == "look_up") binding = "look_down";
                        else if (binding == "look_down") binding = "look_up";
                    }
                    float sensitivity = 1.0f;
                    if (binding.compare(0, 5, "look_") == 0) {
                        sensitivity = joystick.look_sensitivity_;
                    }
                    float depth = iter2.second * sensitivity;

                    int& count = control.key_down[binding].count;
                    int& depth_count = control.key_down[binding].depth_count;
                    if (count <= 0) {
                        count *= -1;
                        ++count;
                        if (depth > KeyState::kDepthThreshold)
                            ++depth_count;
                    }
                    control.key_down[binding].depth = depth;
                }
            }
        }
    }
    // Gameplay follows the keyboard's joined player. Menu navigation also
    // reaches P1 so the couch can manage its lobby using the keyboard.
    const bool catch_kbmouse = controller_id == GetLocalPlayerInputID(keyboard_local_player_);
    if (catch_kbmouse || controller_id == 0) {
        static const std::string kKey = "key";
        const StrMap& key_map = bindings_[kKey];
        for (const auto& iter : key_map) {
            if (!catch_kbmouse && (!keyboard_menu_shared_ || iter.first.compare(0, 5, "menu_") != 0)) continue;
            if (IsKeyDown(iter.second.c_str())) {
                PlayerInputKeyDown(&control.key_down[iter.first]);
                use_controller_input_ = false;
            }
        }
        if (controller_id == 0) keyboard_.Update(timestep);

        if (catch_kbmouse && in_focus_) {
            control.key_down["look_right"].depth += mouse_.delta_[0] * mouse_sensitivity_ * (invert_x_mouse_look_ ? -1.0f : 1.0f);
            control.key_down["look_down"].depth += mouse_.delta_[1] * mouse_sensitivity_ * (invert_y_mouse_look_ ? -1.0f : 1.0f);
            mouse_.delta_[0] = 0;
            mouse_.delta_[1] = 0;
        }
    }
    ApplyInjectedInput(control, controller_id);
    // Zero all negative keydown counts
    for (auto& iter : kd) {
        if (iter.second.count < 0) {
            iter.second.count = 0;
            iter.second.depth_count = 0;
            iter.second.depth = 0.0f;
        }
    }
}

void Input::RecordKeyboardJoinEvent(const SDL_Event& event) {
    if (event.type == SDL_KEYDOWN && !event.key.repeat && in_focus_ &&
        (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER)) {
        keyboard_join_pressed_ = true;
    }
}

bool Input::KeyboardJoinPressed() {
    const bool pressed = keyboard_join_pressed_;
    keyboard_join_pressed_ = false;
    return pressed;
}

Keyboard& Input::getKeyboard() {
    return keyboard_;
}

Mouse& Input::getMouse() {
    return mouse_;
}

void Input::UseRawInput(bool val) {
    use_raw_input = val;
}

void Input::StartTextInput() {
    SDL_StartTextInput();
}

void Input::StopTextInput() {
    SDL_StopTextInput();
}

std::string Input::GetStringDescriptionForBinding(const std::string& type, const std::string& name) {
    StrMap& keyboard_map = bindings_[type];
    StrMap::iterator keyit = keyboard_map.find(name);

    if (keyit != keyboard_map.end()) {
        return StringFromInput(keyit->second.c_str());
    } else {
        return name;
    }
}

std::vector<std::string> Input::GetAvailableBindingCategories() {
    std::vector<std::string> categs;
    BindMap::iterator bindit = bindings_.begin();
    while (bindit != bindings_.end()) {
        categs.push_back(bindit->first);

        bindit++;
    }
    return categs;
}

std::vector<std::string> Input::GetAvailableBindings(const std::string& binding_category) {
    std::vector<std::string> binds;
    StrMap bindings = bindings_[binding_category];
    StrMap::iterator bindit = bindings.begin();
    while (bindit != bindings.end()) {
        binds.push_back(bindit->first);

        bindit++;
    }
    return binds;
}

std::set<std::string> Input::GetAllAvailableBindings() {
    std::set<std::string> binds;

    for (const auto& binding_cat : bindings_) {
        for (const auto& binding : binding_cat.second) {
            binds.insert(binding.first);
        }
    }

    return binds;
}

std::string Input::GetBindingValue(const std::string& binding_category, const std::string& binding) {
    // Player-specific categories inherit individual missing bindings, including
    // multi-digit player indices used by large local sessions.
    if (IsPlayerGamepadCategory(binding_category)) {
        BindMap::iterator iter = bindings_.find(binding_category);
        if (iter != bindings_.end()) {
            StrMap::const_iterator found = iter->second.find(binding);
            if (found != iter->second.end()) {
                return found->second;
            }
        }
        return bindings_["gamepad"][binding];
    } else {
        return bindings_[binding_category][binding];
    }
}

void Input::SetBindingValue(std::string binding_category, std::string binding, std::string value) {
    config.GetRef(binding_category + "[" + binding + "]") = value;
    config.ReloadStaticSettings();
}

void Input::SetKeyboardBindingValue(std::string binding_category, std::string binding, SDL_Scancode value) {
    SetBindingValue(binding_category, binding, std::string(SDLScancodeToString(value)));
}

void Input::SetMouseBindingValue(std::string binding_category, std::string binding, uint32_t button) {
    char buffer[32];
    sprintf(buffer, "mouse%d", button);
    SetBindingValue(binding_category, binding, buffer);
}

void Input::SetMouseBindingValue(std::string binding_category, std::string binding, std::string text) {
    SetBindingValue(binding_category, binding, text.c_str());
}

void Input::SetControllerBindingValue(std::string binding_category, std::string binding, ControllerInput::Input input) {
    SetBindingValue(binding_category, binding, StringFromControllerInput(input));
}

void Input::HandleMouseButtonDown(int sdl_button_index, int clicks) {
    mouse_.MouseDownEvent(sdl_button_index);
}

void Input::HandleMouseButtonUp(int sdl_button_index) {
    mouse_.MouseUpEvent(sdl_button_index);
}

PlayerInput* Input::GetController(int id) {
    return id >= 0 && static_cast<size_t>(id) < player_inputs_.size() ? &player_inputs_[id] : NULL;
}

void Input::ProcessBindings() {
    for (auto& open_joystick : open_joysticks_) {
        RC_JoystickStruct js = open_joystick.second;
        js->joystick.ClearBinding();
        if (js->player_input < 0) {
            continue;
        }
        char gamepad_name[32];
        FormatString(gamepad_name, 32, "gamepad_%i", LocalPlayerIndexForInputID(js->player_input));
        if (!js->gamepad_bind.empty()) {
            StrMap& gamepad_map = bindings_[gamepad_name];
            for (auto& xb_iter : gamepad_map) {
                const std::string& input_str = xb_iter.second;
                ControllerInput::Input input = SDLStringToController(input_str.c_str());
                if (input != ControllerInput::NONE) {
                    SDL_GameControllerButtonBind bind;
                    // Input comes as an internal value, so map it to a physical
                    // (SDL) input, but send the internal input to ProcessBinding
                    switch (input) {
                        case ControllerInput::L_STICK_XN:
                        case ControllerInput::L_STICK_XP:
                            bind = js->gamepad_bind[ControllerInput::L_STICK_X];
                            break;
                        case ControllerInput::L_STICK_YN:
                        case ControllerInput::L_STICK_YP:
                            bind = js->gamepad_bind[ControllerInput::L_STICK_Y];
                            break;
                        case ControllerInput::R_STICK_XN:
                        case ControllerInput::R_STICK_XP:
                            bind = js->gamepad_bind[ControllerInput::R_STICK_X];
                            break;
                        case ControllerInput::R_STICK_YN:
                        case ControllerInput::R_STICK_YP:
                            bind = js->gamepad_bind[ControllerInput::R_STICK_Y];
                            break;
                        default:
                            bind = js->gamepad_bind[input];
                            break;
                    }
                    if (bind.bindType != SDL_CONTROLLER_BINDTYPE_NONE) {
                        js->joystick.ProcessBinding(input, xb_iter.first);
                    }
                }
            }
        } else {
            // TODO: Generic controller support
            /*const StrMap &map = bindings_["controller"];
            for( StrMap::const_iterator iter = map.begin(); iter != map.end(); ++iter ) {
                js->joystick.ProcessBinding(iter->second, iter->first);
            }*/
        }
    }
}

void Input::ProcessControllers(float timestep) {
    PROFILER_ZONE(g_profiler_ctx, "ProcessControllers");
    for (unsigned i = 0; i < player_inputs_.size(); ++i) {
        ProcessController(i, timestep);
    }
}

/*
 * Poll all KeyboardListeners, asking if they want to the exclusive input this frame.
 */
void Input::UpdateKeyboardFocus() {
    uint32_t mask = 0U;
    if (mask == 0U) {
        mask = KIMF_ANY;
    }
    keyboard_.SetMode(mask);
}

void Input::AssignLocalJoysticks(bool reset) {
    const unsigned local_slots = static_cast<unsigned>(local_player_inputs_.size());
    std::set<int> assigned;
    // Offline single player accepts any pad. Online device claims belong to
    // the lobby: hotplug must not temporarily give an unknown pad P1's input.
    for (auto& entry : open_joysticks_) {
        JoystickStruct& js = *entry.second.operator->();
        const int previous = js.player_input;
        if (local_slots == 1 && !preserve_remote_inputs_) {
            js.player_input = 0;
        } else if ((!preserve_remote_inputs_ && reset) || LocalPlayerIndexForInputID(js.player_input) < 0 ||
                   !assigned.insert(js.player_input).second) {
            js.player_input = -1;
        }
        if (previous != js.player_input) {
            js.joystick.buttons_down_.clear();
        }
    }
    if (local_slots > 1 && !preserve_remote_inputs_) {
        unsigned next = reset && open_joysticks_.size() >= local_slots ? 0 : 1;
        for (auto& entry : open_joysticks_) {
            JoystickStruct& js = *entry.second.operator->();
            if (js.player_input >= 0) {
                continue;
            }
            while (next < local_slots && assigned.count(GetLocalPlayerInputID(next))) {
                ++next;
            }
            if (next < local_slots) {
                js.player_input = GetLocalPlayerInputID(next++);
            } else if (!assigned.count(0)) {
                js.player_input = 0;
            }
            if (js.player_input >= 0) {
                assigned.insert(js.player_input);
            }
            js.joystick.buttons_down_.clear();
        }
    }
    UpdateGamepadLookSensitivity();
    UpdateGamepadDeadzone();
    ProcessBindings();
    joystick_assignment_dirty_ = false;
}

int Input::GetLocalPlayerInputID(unsigned local_index) const {
    return local_index < local_player_inputs_.size() ? local_player_inputs_[local_index] : -1;
}

int Input::LocalPlayerIndexForInputID(int input_id) const {
    auto found = std::find(local_player_inputs_.begin(), local_player_inputs_.end(), input_id);
    return found == local_player_inputs_.end() ? -1 : static_cast<int>(found - local_player_inputs_.begin());
}

void Input::BindLocalJoystick(int instance_id, int local_index) {
    auto found = open_joysticks_.find(instance_id);
    if (found == open_joysticks_.end()) return;
    const int input_id = local_index < 0 ? -1 : GetLocalPlayerInputID(static_cast<unsigned>(local_index));
    if (found->second->player_input == input_id) return;
    found->second->player_input = input_id;
    found->second->joystick.buttons_down_.clear();
    ProcessBindings();
    UpdateGamepadLookSensitivity();
    UpdateGamepadDeadzone();
}

void Input::BindLocalKeyboard(int local_index) {
    keyboard_local_player_ = local_index >= 0 &&
        static_cast<size_t>(local_index) < local_player_inputs_.size() ? local_index : -1;
    mouse_.delta_[0] = mouse_.delta_[1] = 0;
}

void Input::RemoveLocalPlayerInput(unsigned local_index) {
    const unsigned count = static_cast<unsigned>(local_player_inputs_.size());
    if (count <= 1 || local_index >= count) return;
    const Config saved_config = config;
    auto prefix = [](unsigned index) { return "gamepad_" + std::to_string(index); };
    auto is_profile = [](const std::string& key, const std::string& start) {
        return key.compare(0, start.size(), start) == 0 && key.size() > start.size() &&
            (key[start.size()] == '_' || key[start.size()] == '[');
    };
    for (unsigned index = local_index; index < count; ++index) {
        const std::string target = prefix(index), source = prefix(index + 1);
        for (auto it = config.map_.begin(); it != config.map_.end();) {
            if (is_profile(it->first, target)) it = config.map_.erase(it);
            else ++it;
        }
        bindings_.erase(target);
        if (index + 1 < count) {
            for (const auto& entry : saved_config.map_) {
                if (is_profile(entry.first, source)) {
                    config.map_[target + entry.first.substr(source.size())] = entry.second;
                }
            }
        }
    }
    local_player_inputs_.erase(local_player_inputs_.begin() + local_index);
    for (const char* key : {"controller_menu_owner", "menu_player_config"}) {
        auto owner = config.map_.find(key);
        if (owner == config.map_.end()) continue;
        const int previous = owner->second.data.toNumber<int>();
        if (previous > static_cast<int>(local_index)) owner->second.data = previous - 1;
        else if (previous == static_cast<int>(local_index)) owner->second.data = 0;
    }
    if (keyboard_local_player_ == static_cast<int>(local_index)) keyboard_local_player_ = -1;
    else if (keyboard_local_player_ > static_cast<int>(local_index)) --keyboard_local_player_;
    SetUpForXPlayers(count - 1, true);
    SetFromConfig(config);
}

void Input::SetUpForXPlayers(unsigned num_players, bool preserve_remote) {
    const bool changed = num_players_ != num_players || preserve_remote_inputs_ != preserve_remote;
    if (!changed && !joystick_assignment_dirty_) {
        return;
    }
    const bool first_split = num_players_ <= 1 && num_players > 1;
    num_players_ = num_players;
    preserve_remote_inputs_ = preserve_remote;
    if (!preserve_remote) keyboard_local_player_ = 0;
    // Slot zero also drives menus when no level is loaded. Retain the original
    // four reserved slots so the standalone online controller IDs stay stable.
    const unsigned local_slots = std::max(1u, num_players);
    if (player_inputs_.size() < local_slots) {
        player_inputs_.resize(local_slots);
    }
    std::vector<int> local_ids;
    local_ids.reserve(local_slots);
    for (unsigned i = 0; i < local_slots; ++i) {
        int id = !preserve_remote || i >= local_player_inputs_.size() ? static_cast<int>(i) : local_player_inputs_[i];
        if (preserve_remote && (id < 0 || player_inputs_[id].remote_controlled ||
                                std::find(local_ids.begin(), local_ids.end(), id) != local_ids.end())) {
            id = static_cast<int>(player_inputs_.size());
            player_inputs_.emplace_back();
        }
        local_ids.push_back(id);
    }
    local_player_inputs_.swap(local_ids);
    for (size_t i = 0; i < player_inputs_.size(); ++i) {
        PlayerInput& control = player_inputs_[i];
        if (LocalPlayerIndexForInputID(static_cast<int>(i)) >= 0) {
            // Offline can follow an online session, whose remote slots are no
            // longer in use. Online only requests one local slot.
            if (control.remote_controlled || !control.enabled) {
                control.key_down.clear();
            }
            control.remote_controlled = false;
            control.enabled = true;
        } else if (!control.remote_controlled) {
            control.enabled = false;
            control.key_down.clear();
            injected_player_keys_.erase(static_cast<int>(i));
            injected_player_look_.erase(static_cast<int>(i));
        }
    }
    for (unsigned i = 0; i < std::max(4u, local_slots); ++i) {
        CompleteGamepadBindings("gamepad_" + std::to_string(i), bindings_);
    }
    AssignLocalJoysticks(first_split);
}

void Input::RequestQuit() {
    quit_requested_ = true;
}

bool Input::WasQuitRequested() {
    return quit_requested_;
}

void CheckBinding(const Config::Map::const_iterator& iter, const std::string& type, BindMap& bind_map) {
    std::string find_str = type + "[";
    size_t start = iter->first.find(find_str, 0);
    if (start != std::string::npos) {
        size_t end = iter->first.find("]", 0);
        start += find_str.size();
        std::string label = iter->first.substr(start, end - start);
        std::string binding = iter->second.data.str();
        bind_map[type][label] = binding;
    }
}

static void CompleteGamepadBindings(const std::string& type, BindMap& bind_map) {
    StrMap& gamepad = bind_map[type];
    for (auto& iter : bind_map["gamepad"]) {
        StrMap::iterator gamepad_iter = gamepad.find(iter.first);
        if (gamepad_iter == gamepad.end()) {
            gamepad[iter.first] = iter.second;
        }
    }
    char buffer[128];
    FormatString(buffer, 128, "%s_deadzone", type.c_str());
    if (!config.HasKey(buffer) || config[buffer].raw().empty()) {
        if (!config["gamepad_deadzone"].raw().empty()) config.GetRef(std::string(buffer)) = config["gamepad_deadzone"];
    }
    FormatString(buffer, 128, "%s_look_sensitivity", type.c_str());
    if (!config.HasKey(buffer) || config[buffer].raw().empty() || config[buffer].toNumber<float>() <= 0.0f) {
        const float inherited = config["gamepad_look_sensitivity"].toNumber<float>();
        if (inherited > 0.0f) config.GetRef(std::string(buffer)) = inherited;
    }
    for (const char* axis : {"x", "y"}) {
        std::string key = type + "_invert_" + axis + "_look";
        if (!config.HasKey(key)) {
            config.GetRef(key) = false;
        }
    }
}

void Input::SetFromConfig(const Config& config) {
    bindings_.clear();
    for (Config::Map::const_iterator iter = config.map_.begin(); iter != config.map_.end(); ++iter) {
        CheckBinding(iter, "key", bindings_);
        CheckBinding(iter, "gamepad", bindings_);
        const size_t bracket = iter->first.find('[');
        if (bracket != std::string::npos) {
            const std::string category = iter->first.substr(0, bracket);
            if (IsPlayerGamepadCategory(category)) {
                CheckBinding(iter, category, bindings_);
            }
        }
        CheckBinding(iter, "controller", bindings_);
        CheckBinding(iter, "bind", bindings_);
        CheckBinding(iter, "bind_win", bindings_);
        CheckBinding(iter, "bind_unix", bindings_);
    }
    for (unsigned i = 0; i < std::max(4u, num_players_); ++i) {
        CompleteGamepadBindings("gamepad_" + std::to_string(i), bindings_);
    }
    for (const auto& category : bindings_) {
        if (IsPlayerGamepadCategory(category.first)) {
            CompleteGamepadBindings(category.first, bindings_);
        }
    }
    use_raw_input = config["use_raw_input"].toNumber<bool>();
    // Process hotkey bindings
    StrMap& binds = bindings_["bind"];
    for (StrMap::const_iterator iter = binds.begin(); iter != binds.end(); ++iter) {
        KeyCommand::BindString((iter->second + ": " + iter->first).c_str());
    }
#ifdef WIN32
    StrMap& platform_binds = bindings_["bind_win"];
#else
    StrMap& platform_binds = bindings_["bind_unix"];
#endif
    for (StrMap::const_iterator iter = platform_binds.begin(); iter != platform_binds.end(); ++iter) {
        KeyCommand::BindString((iter->second + ": " + iter->first).c_str());
    }
    KeyCommand::FinalizeBindings();
    mouse_sensitivity_ = config["mouse_sensitivity"].toNumber<float>();
    UpdateGamepadLookSensitivity();
    UpdateGamepadDeadzone();
    SetInvertXMouseLook(config["invert_x_mouse_look"].toNumber<bool>());
    SetInvertYMouseLook(config["invert_y_mouse_look"].toNumber<bool>());
    debug_keys = config["debug_keys"].toNumber<bool>();
    if (!open_joysticks_.empty()) {
        ProcessBindings();
    }
}

void Input::ClearQuitRequested() {
    quit_requested_ = false;
}

int Input::PlayerOpenedMenu() {
    for (size_t i = 0; i < player_inputs_.size(); ++i) {
        if (player_inputs_[i].enabled && !player_inputs_[i].remote_controlled &&
            player_inputs_[i].key_down["quit"].count == 1) {
            return i;
        }
    }

    return -1;
}

void Input::AllowControllerInput(bool allow) {
    allow_controller_input_ = allow;
}

int Input::AllocateRemotePlayerInput() {
    int index = player_inputs_.size();
    player_inputs_.push_back(PlayerInput());
    player_inputs_[index].remote_controlled = true;
    player_inputs_[index].enabled = true;
    return index;
}

bool Input::IsKeyDown(const char* name) {
    if (strlen(name) <= 5 || memcmp(name, "mouse", 5) != 0) {
        return keyboard_.isScancodeDown(StringToSDLScancode(name), KIMF_PLAYING);
    } else if (strcmp(name, "mousescrollup") == 0) {
        return mouse_.wheel_delta_y_ > 0;
    } else if (strcmp(name, "mousescrolldown") == 0) {
        return mouse_.wheel_delta_y_ < 0;
    } else if (strcmp(name, "mousescrollleft") == 0) {
        return mouse_.wheel_delta_x_ < 0;
    } else if (strcmp(name, "mousescrollright") == 0) {
        return mouse_.wheel_delta_x_ > 0;
    } else {
        int button = atoi(name + 5);
        if (button >= 0 && button < Mouse::MouseButton::NUM_BUTTONS)
            return mouse_.mouse_down_[button] == Mouse::ClickState::HELD;
        else {
            LOGW << "Unknown mouse button \"" << name << "\"" << std::endl;
            return false;
        }
    }
}

bool Input::IsControllerConnected() {
    return !open_joysticks_.empty();
}

Input::JoystickMap& Input::GetOpenJoysticks() {
    return this->open_joysticks_;
}

std::vector<Keyboard::KeyboardPress> Input::GetKeyboardInputs() {
    return keyboard_.GetKeyboardInputs();
}

std::vector<Mouse::MousePress> Input::GetMouseInputs() {
    return mouse_.GetMouseInputs();
}

std::vector<Joystick::JoystickPress> Input::GetJoystickInputs(int player_index) {
    std::vector<Joystick::JoystickPress> ret_inputs;
    const int input_id = player_index < 0 ? -1 : GetLocalPlayerInputID(static_cast<unsigned>(player_index));
    for (auto& open_joystick : open_joysticks_) {
        if (input_id >= 0 && open_joystick.second->player_input == input_id) {
            std::vector<Joystick::JoystickPress> inputs = open_joystick.second->joystick.GetJoystickInputs();
            ret_inputs.reserve(ret_inputs.size() + inputs.size());
            for (auto& input : inputs) {
                ret_inputs.push_back(input);
            }
        }
    }
    // Some insertion sort variant
    for (int i = 1; i < (int)ret_inputs.size(); ++i) {
        Joystick::JoystickPress curr = ret_inputs[i];
        int j = i - 1;
        for (; j >= 0 && ret_inputs[j].s_id > curr.s_id; --j) {
            ret_inputs[j + 1] = ret_inputs[j];
        }
        ret_inputs[j + 1] = curr;
    }
    return ret_inputs;
}
