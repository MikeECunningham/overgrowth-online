// Per-seat experience readout and progression shop. Apache-2.0.
#pragma once
#include <JSON/json.h>
#include <Math/vec2.h>
class Engine;
namespace ProgressionUI {
void Draw(Engine& engine);
void Clear();
vec2 LayoutArenaScore(float left, float top, float width, float height);
vec2 LayoutArenaAnnouncement(float top, float width, float height);
const Json::Value& Inspect();
}
