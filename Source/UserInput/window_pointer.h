// Absolute SDL pointer used by menus and their cursors. Apache-2.0.
#pragma once

#include <Math/vec2.h>
#include <cstdint>

struct WindowPointer {
    int x = 0, y = 0;  // SDL window coordinates, origin at top left
    uint32_t buttons = 0;
    bool available = false;

    bool MenuPosition(vec2& position) const;
};

WindowPointer ReadWindowPointer();
