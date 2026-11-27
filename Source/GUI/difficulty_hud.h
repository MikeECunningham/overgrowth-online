// Host-view difficulty conditions. Apache-2.0.
#pragma once

#include <JSON/json.h>
#include <Math/vec2.h>

class Engine;

namespace DifficultyHUD {
void BeginFrame();
void Dispose();
void Draw(Engine& engine);
// Space at the right and bottom of this player's viewport occupied by the Survival shop readout.
vec2 ShopInset(unsigned player);
const Json::Value& Inspect();
}
