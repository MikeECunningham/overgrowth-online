//-----------------------------------------------------------------------------
//           Name: raytrace.h
//      Developer: Overgrowth Online (crxhvrd)
//    Description: Ray tracing in the frame (the Vulkan renderer, a device with ray
//                 queries; VkRt does the tracing). The sun's shadow (config
//                 rt_shadows) and the ambient occlusion (rt_ao), traced from the depth
//                 prepass for each pixel, denoised (rt_denoise), into a mask the colour
//                 pass reads (envobject #RT_LIGHTING, TEX_RT_LIGHTING) instead of its
//                 shadow map, and over its ambient light. The scene traced: static
//                 meshes, terrain, items and characters
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#pragma once

#include <Math/mat4.h>
#include <Math/ivec2.h>

class Camera;
class SceneGraph;

namespace Raytrace {

// Ray-traced lighting asked for (rt_shadows, rt_ao, rt_light_shadows, rt_reflections or rt_gi) and able to run here
bool Wanted();

// The grass in the depth prepass (rt_grass, ray tracing wanted): the rays start from its
// blades, which the colour pass then reads as its own
bool GrassInPrepass();
// How far from the camera grass is in the RT scene (rt_grass_range): the prepass draws only that
float GrassRange();

// Engine::DrawScene, after the depth prepass of a single screen's final frame: the scene
// gathered, traced and denoised at the lighting resolution, the mask bound for the colour pass
void TraceLighting(SceneGraph* scenegraph, Camera* camera, const mat4& proj, const mat4& view, int width, int height);
// The colour pass of this scene reads the mask (SceneGraph's shader suffix: #RT_LIGHTING)
bool MaskReady();
ivec2 LightingSize();
// ... and traces its reflections (rt_reflections: #RT_REFLECTIONS, rt_reflect.glsl)
bool ReflectionsReady();
// ... and takes its ambient light from the traced GI (rt_gi: #RT_GI, TEX_RT_GI)
bool GiReady();
// The scene drawn: the next one traces again
void EndScene();
// Release full-screen lighting targets when a level or single-viewport RT ends.
void ReleaseTargets();

}  // namespace Raytrace
