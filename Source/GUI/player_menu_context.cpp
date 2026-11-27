#include "player_menu_context.h"
#include <GUI/IMUI/im_support.h>
#include <Graphics/graphics.h>
#include <Internal/config.h>
#include <UserInput/input.h>
#include <UserInput/window_pointer.h>
#include <algorithm>

namespace PlayerMenuUI {
namespace { const Context* current = nullptr; }

const Context* Current() { return current; }
int Width() { return current ? current->width : Graphics::Instance()->window_dims[0]; }
int Height() { return current ? current->height : Graphics::Instance()->window_dims[1]; }
bool AllowsMouse() { return !current || current->mouse_enabled; }
bool AllowsKeyboard() { return !current || current->local_index == Input::Instance()->LocalKeyboardPlayerIndex(); }
bool LocalMousePosition(const Context& viewport, int window_height, vec2& position, bool dragging) {
    if (!viewport.mouse_enabled || viewport.width <= 0 || viewport.height <= 0) return false;
    position.x() -= viewport.x;
    position.y() -= window_height - viewport.y - viewport.height;
    if (dragging) {
        // Keep the crossed axis just outside its edge for hit testing. A slider
        // still tracks motion along the other axis as the pointer travels past it.
        position.x() = std::max(-1.0f, std::min(position.x(), float(viewport.width)));
        position.y() = std::max(-1.0f, std::min(position.y(), float(viewport.height)));
        return true;
    }
    return position.x() >= 0 && position.x() < viewport.width && position.y() >= 0 && position.y() < viewport.height;
}

bool MouseTopLeft(vec2& position, bool dragging) {
    if (!AllowsMouse()) return false;
    Input* input = Input::Instance();
    if (input->GetGrabMouse()) {
        const Mouse& mouse = input->getMouse();
        position = vec2(float(mouse.pos_[0]), float(mouse.pos_[1]));
    } else if (!ReadWindowPointer().MenuPosition(position)) {
        return false;
    }
    return !current || LocalMousePosition(*current, Graphics::Instance()->window_dims[1], position, dragging);
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
    if (context_.width <= 0 || context_.height <= 0) context_.mouse_enabled = false;
    context_.width = std::max(1, context_.width);
    context_.height = std::max(1, context_.height);
    current = &context_;
    // These are temporary call metadata, not persistent settings edits. GetRef
    // would mark the config dirty on every Update/Draw of every open menu.
    config.map_["controller_menu_owner"].data = context_.local_index;
    config.map_["menu_player_config"].data = context_.local_index;
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
