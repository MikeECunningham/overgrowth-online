// Per-call UI state for an independently owned local player's menu.
#pragma once

#include <Math/vec2.h>
#include <memory>

namespace PlayerMenuUI {
struct Context {
    int input_id = 0;
    int local_index = 0;
    int x = 0, y = 0, width = 1, height = 1;  // bottom-left window pixels
    bool mouse_enabled = false;
};

const Context* Current();
int Width();
int Height();
bool AllowsMouse();
bool AllowsKeyboard();
vec2 MouseTopLeft();

class Scope {
    struct Saved;
    std::unique_ptr<Saved> saved_;
    Context context_;
public:
    explicit Scope(const Context& context);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}
