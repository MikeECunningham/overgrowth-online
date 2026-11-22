#include "player_menu_context.h"
#include <GUI/IMUI/im_support.h>
#include <Graphics/graphics.h>
#include <Internal/config.h>
#include <UserInput/input.h>
#include <algorithm>

namespace PlayerMenuUI {
namespace { const Context* current = nullptr; }

const Context* Current() { return current; }
int Width() { return current ? current->width : Graphics::Instance()->window_dims[0]; }
int Height() { return current ? current->height : Graphics::Instance()->window_dims[1]; }
bool AllowsMouse() { return !current || current->mouse_enabled; }
bool AllowsKeyboard() { return !current || current->local_index == Input::Instance()->LocalKeyboardPlayerIndex(); }
vec2 MouseTopLeft() {
    const Mouse& mouse = Input::Instance()->getMouse();
    if (!current) return vec2(static_cast<float>(mouse.pos_[0]), static_cast<float>(mouse.pos_[1]));
    if (!AllowsMouse()) return vec2(-100000.0f);
    const int top = Graphics::Instance()->window_dims[1] - current->y - current->height;
    const int x = mouse.pos_[0] - current->x, y = mouse.pos_[1] - top;
    if (x < 0 || y < 0 || x >= current->width || y >= current->height) return vec2(-100000.0f);
    return vec2(static_cast<float>(x), static_cast<float>(y));
}

struct Scope::Saved {
    const Context* previous;
    ScreenMetrics metrics;
    ConfigVal owner, profile;
    bool had_owner, had_profile;
    Saved() : previous(current), metrics(screenMetrics),
        had_owner(config.HasKey("controller_menu_owner")), had_profile(config.HasKey("menu_player_config")) {
        if (had_owner) owner = config.map_.at("controller_menu_owner");
        if (had_profile) profile = config.map_.at("menu_player_config");
    }
};

Scope::Scope(const Context& context) : saved_(new Saved()), context_(context) {
    context_.width = std::max(1, context_.width);
    context_.height = std::max(1, context_.height);
    current = &context_;
    config.GetRef("controller_menu_owner") = context_.local_index;
    config.GetRef("menu_player_config") = context_.local_index;
    screenMetrics.computeFactors();
    screenMetrics.mainSize = screenMetrics.GUISpace;
}

Scope::~Scope() {
    current = saved_->previous;
    screenMetrics = saved_->metrics;
    if (saved_->had_owner) config.map_["controller_menu_owner"] = saved_->owner;
    else config.map_.erase("controller_menu_owner");
    if (saved_->had_profile) config.map_["menu_player_config"] = saved_->profile;
    else config.map_.erase("menu_player_config");
}
}
