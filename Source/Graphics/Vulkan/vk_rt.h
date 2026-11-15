//-----------------------------------------------------------------------------
//           Name: vk_rt.h
//      Developer: Overgrowth Online (crxhvrd)
//    Description: Hardware ray tracing on the Vulkan renderer: ray queries in our own
//                 compute passes. A BLAS per mesh (its model's own vertices and faces,
//                 made the first time the mesh is drawn and kept while it's used), the
//                 scene's instances in a TLAS built for each scene drawn, and the passes
//                 that trace it: the sun's shadow, the ambient occlusion and the point
//                 lights' shadows, then their
//                 denoiser. Recorded into the frame's commands between the passes, as the
//                 upscalers are
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#pragma once

#include <opengl.h>
#include <volk.h>

#include <cstdint>

class Model;

namespace VkRt {

// Ray queries can run here: the device has them and our passes compiled (why not is
// logged once)
bool Available();

// What a mesh's surface is to the rays that meet it
struct Surface {
    GLuint texture = 0;         // its colour texture (0: none; reflections see it grey)
    bool alpha_tested = false;  // a ray passes where that texture's alpha is under 0.5
    float scroll = 0.0f;        // the texture moves down by this much a second (envobject #SCROLL_*: water)
    bool double_sided = false;  // drawn without culling: its faces turned away from a point light shadow it too (an alpha-tested one's always do)
    bool emitter = false;       // it glows (lava, #GLOW, #EMISSIVE): the point lights in and around it are its glow, so it doesn't shadow them
    float emission = 0.0f;      // ... how bright, by its colour (as its draw: #MAGMA_FLOW's about 0.8, #GLOW's 5)
    bool waterfall = false;     // envobject #WATERFALL: reflections light it as its draw does (facing up)
    bool grass = false;         // grass, a detail object: instance mask 0x10 (it shadows the sun and bounces light, but no AO or lamp rays meet it)
    // Its colour tint (the editor's, as its draw tints its colour: instance_color_tint),
    // through `tint_mask`'s alpha (its normal map's, as the draw has it; 0: the whole of it)
    float tint[3] = {1.0f, 1.0f, 1.0f};
    GLuint tint_mask = 0;
};

// The scene to trace, gathered again for each scene drawn: a mesh in object space (the
// model's vertices and faces) where `transform` (column-major 4x4) puts it
void BeginScene();
void AddMesh(const Model& model, const float transform[16], const Surface& surface);
// ... many of one mesh, one material for them all (grass: `count` transforms of 16 floats)
void AddMeshInstances(const Model& model, const float* transforms, size_t count, const Surface& surface);
// A skinned mesh (a character): the model's bind pose through `bones` (column-major
// 4x4s to world space, as its bone ids index them) and its morphs' offsets, skinned on
// the GPU for each scene, its BLAS refitted. `owner` keeps characters sharing a model
// apart; `texture` its colour texture
void AddSkinnedMesh(const void* owner, const Model& model, const float* bones, size_t bone_count, GLuint texture);
// A plant (envobject #PLANT): placed as its draw places it, `transform` (column-major) =
// translation * rotation (its bend in) * scale. `deform`: moved as its draw moves it too
// (the wind and a push's shake, by the model's per-vertex stability, at `time`), in its own
// BLAS refitted each scene; else that transform on the model's shared BLAS
struct PlantPose {
    float transform[16] = {};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // a quaternion: x, y, z, w
    float translation[3] = {};
    float time = 0.0f;
    float shake = 0.0f;
    float scroll = 0.0f;     // (as AddMesh's)
    bool waterfall = false;  // envobject #WATERFALL: the wind's shake at a fixed 0.1, not by the stability, not turned
};
void AddPlant(const void* owner, const Model& model, const PlantPose& pose, const Surface& surface, bool deform);
int SceneInstances();  // (diagnostics: the meshes added since BeginScene)

struct LightingPass {
    GLuint depth = 0;  // the depth prepass's (reversed-Z, 0 at the far plane; multisampled or not)
    // The scene's size, storage usage each: the raw result (RGBA32F: sun, depth, AO,
    // lights), this frame's and last frame's; the denoiser's history (RGBA16F: sun, AO,
    // frames in it, lights), last frame's and this frame's; the mask the colour pass reads
    // (RGBA32F: r the sun's visibility, -1 not traced; g the depth traced from; b the
    // ambient light left after occlusion, 1 not traced; a the share of the point lights'
    // light that gets there, 1 not traced); what sizes the penumbras (RG16F: the sun's
    // blocker distance, the lights' penumbra width)
    GLuint raw = 0, raw_prev = 0, history_prev = 0, history = 0, mask = 0, extra = 0;
    uint32_t width = 0, height = 0;
    float inv_view_proj[16] = {};  // clip space (depth 0..1) to world, column-major, as drawn (jitter and all)
    float reprojection[16] = {};   // this frame's clip space to last frame's (has_history)
    bool has_history = false;      // last frame's raw and history are this camera's, there to go on from
    bool denoise = true;
    float camera[3] = {};          // the camera's position
    float near_plane = 0.1f;
    float pixel_angle = 0.001f;    // a pixel's angle, radians (the vertical field of view over the height)
    float sun[3] = {};             // toward the sun, normalised
    float cone = 0.0f;             // the sun's angular radius, radians (0: hard)
    int sun_rays = 1;              // per pixel (0: the sun isn't traced)
    int ao_rays = 0;               // per pixel (0: no ambient occlusion)
    float ao_range = 1.5f;         // how far the AO's rays look, world units
    uint32_t frame = 0;            // the sequence the rays are spread over
    float bias = 0.0f;             // the ray's start off the surface per unit of its distance
    float time = 0.0f;             // the draw's (scrolling textures)
    // The point lights, as the colour pass has them: 8 floats each (x, y, z, how far its
    // light reaches, r, g, b, its luminance)
    const float* lights = nullptr;
    uint32_t light_count = 0;
    int light_rays = 0;            // per pixel (0: their shadows aren't traced)
    float light_size = 0.15f;      // a light's radius, world units (its shadows' softness)
    float light_clearance = 0.25f; // how far short of a light its rays stop (its lamp's housing), world units
    // NVIDIA's denoisers (VkNrd) in place of ours, where they're there (`denoise` on too):
    // world to clip (depth 0..1) unjittered, this frame's and last frame's (the motion);
    // NRD's matrices (VkNrd::Frame); its history starting over
    bool nrd = false;
    bool nrd_reset = false;
    // Global illumination (rt_gi.comp, its radiance cache; denoised by NRD's REBLUR, as
    // spherical harmonics so the colour pass resolves it with its own normals): the raw
    // result (RGBA16F storage: radiance, hit distance; the ray's direction), what the colour
    // pass reads (RGBA16F: REBLUR's SH0 (YCoCg, hit distance) and SH1 (direction * Y)), the
    // sky's cube map (the colour pass's spec cube map: rays that meet nothing), the sun's
    // light (the colour pass's primary_light_color.rgb * a); the cache emptied (a level
    // changed); debug: 1 no cache lookups (one bounce), 2 the cache itself at each pixel
    bool gi = false;
    GLuint gi_raw = 0, gi_dir = 0, gi_sh0 = 0, gi_sh1 = 0, sky = 0;
    // GI for DLSS Ray Reconstruction instead (it denoises the frame as it upscales): every
    // pixel traced, not denoised, written as SH into gi_raw and gi_dir (the colour pass reads
    // those)
    bool gi_rr = false;
    uint32_t gi_debug = 0;
    float sun_color[3] = {};
    bool gi_cache_reset = false;
    float gi_voxel = 0.25f;     // the cache's smallest voxel (world units)
    float gi_emission = 1.0f;   // the glowing surfaces' light, by their draws'
    float gi_sky_lod = 3.0f;    // (the sky's cube map, how blurred: rays that meet nothing)
    float gi_sky_scale = 1.0f;
    // the light a pixel receives, scaled (the cache's bounces stay as traced): the levels'
    // probes were baked with the sun at 1.5-3x, so their ambient is brighter than the traced
    float gi_strength = 1.0f;
    // the light a hit gives back, scaled (rt_gi_bounce; not the sky's): the game's bounce
    // was baked at 1.5-3 times the sun
    float gi_bounce = 2.0f;
    // how much of the game's rasterized ambient (its sky cube map or probes, its shadow
    // decals) lights the second bounce's hits (0..1; 0: none, the sky traced alone)
    float gi_raster_ambient = 0.0f;
    // The game's ambient light at a GI ray's hit, as the colour pass lights that surface
    // (raytrace.cpp's State::ambient: a header of 16 words, the light probes' grid, 12 words
    // a point, the level's shadow decals, 16 words each); a new version uploads it.
    // And the colour pass's GetAmbientContrib(1) for this sun (1.5 - its intensity / 2)
    const uint32_t* probe_grid = nullptr;
    size_t probe_grid_words = 0;
    uint64_t probe_grid_version = 0;
    float ambient_contrib = 1.0f;
    bool light_cull = true;     // (diagnostics) the point lights' rays pass the faces turned from the light
    float gi_lights = 0.25f;    // how much of the point lights' light GI bounces (0..1: they're often fill lights already)
    float view_proj[16] = {}, prev_view_proj[16] = {};
    float nrd_view[16] = {}, nrd_proj[16] = {}, nrd_view_prev[16] = {}, nrd_proj_prev[16] = {};
};
// NRD is there to denoise with (built with it, NRD.dll beside the game, the device able)
bool NrdAvailable();
// This scene's GI was traced and denoised into LightingPass::gi_sh0 and gi_sh1
bool GiTraced();

// Draws can trace too (the colour pass's reflections, rt_reflect.glsl): the device reads a
// hit's triangle back (position fetch) and the passes are there
bool DrawsCanTrace();
// This scene's, for draws after TraceLighting till EndScene: its TLAS, its instances'
// materials (rt_reflect.glsl's RtMaterial, by instanceCustomIndex), the texture slots'
// set. False: none this scene (not traced, or ended)
struct DrawScene {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer materials = VK_NULL_HANDLE;
    VkDescriptorSet textures = VK_NULL_HANDLE;
};
bool SceneForDraws(DrawScene* out);
// The texture slots' set layout (set 1 of a program that traces)
VkDescriptorSetLayout TexturesLayout();
// The scene drawn: its TLAS no longer handed to draws
void EndScene();
// The TLAS built, the scene traced and denoised now; false: not (nothing was written)
bool TraceLighting(const LightingPass& pass);

}  // namespace VkRt
