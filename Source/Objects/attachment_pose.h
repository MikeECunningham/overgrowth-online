// Sample a stock item attachment animation in physical bone space. Apache-2.0.
#pragma once
#include <Math/mat4.h>
#include <string>

class Skeleton;
mat4 LoadAttachPose(const std::string& animation, const std::string& chain, const Skeleton& skeleton,
                    int* bone, bool mirrored, bool orient_blade, int chain_depth);
