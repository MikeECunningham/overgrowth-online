// Host-view difficulty conditions. Apache-2.0.
#pragma once

#include <JSON/json.h>

class Engine;

namespace DifficultyHUD {
void BeginFrame();
void Dispose();
void Draw(Engine& engine);
const Json::Value& Inspect();
}
