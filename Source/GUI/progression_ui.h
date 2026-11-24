// Per-seat experience readout and between-round shop. Apache-2.0.
#pragma once
#include <JSON/json.h>
class Engine;
namespace ProgressionUI {
void Draw(Engine& engine);
void Clear();
const Json::Value& Inspect();
}
