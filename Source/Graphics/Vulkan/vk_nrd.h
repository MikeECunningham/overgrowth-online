//-----------------------------------------------------------------------------
//           Name: vk_nrd.h
//      Developer: Overgrowth Online (crxhvrd)
//    Description: NVIDIA's real-time denoisers (NRD) on the Vulkan renderer, for the
//                 ray-traced lighting (VkRt::TraceLighting): SIGMA for the sun's shadow
//                 and for the point lights' (their share of light as its translucency),
//                 REBLUR for the ambient occlusion. NRD hands out its passes (SPIR-V,
//                 resources, constants); this records them, on images of its own at the
//                 scene's size: the inputs rt_nrd.comp #PREPARE writes, the outputs
//                 #COMPOSITE reads. Built with NRD only where its SDK was given
//                 (CMake NRD_SDK: NVIDIA's RTX SDK licence, not in this repository), and
//                 run only where NRD.dll is beside the game; elsewhere our own denoiser
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#pragma once

#include <volk.h>

#include <cstdint>

namespace VkNrd {

// NRD can run here: built with it, NRD.dll there and the version built against, its
// passes made (why not is logged once)
bool Available();

// Its images (GENERAL layout), at the scene's size
enum Image {
    kViewZ,              // R32F: view-space Z (negative ahead); the sky beyond the denoising range
    kNormalRoughness,    // A2B10G10R10: NRD_FrontEnd_PackNormalAndRoughness (oct-packed normal, linear roughness)
    kMotion,             // RGBA16F: 2.5D motion (last frame's uv minus this frame's; last frame's view Z minus this frame's)
    kSunPenumbra,        // R16F: SIGMA_FrontEnd_PackPenumbra
    kLightPenumbra,      // R16F: the point lights' penumbra (as SIGMA's, weighted by the lights)
    kLightTranslucency,  // RGBA8: SIGMA_FrontEnd_PackTranslucency: x all of them reach it, yzw the share of their light that does
    kAoHitDist,          // R16F: REBLUR's normalized hit distance (the noisy AO)
    kSunShadow,          // R8: SIGMA's (its square: the sun's visibility)
    kLightShadow,        // RGBA8: SIGMA's (y squared: the share of the lights' light)
    kAoOut,              // R8: REBLUR's (the AO)
    kGiSh0In,            // RGBA16F: REBLUR_FrontEnd_PackSh's SH0 (GI: YCoCg radiance, normalized hit distance)
    kGiSh1In,            // RGBA16F: ... its SH1 (the direction * Y)
    kGiSh0Out,           // RGBA16F: REBLUR's SH0
    kGiSh1Out,           // RGBA16F: REBLUR's SH1
    kImages
};

struct Frame {
    uint32_t width = 0, height = 0;
    // Column-major, as NRD has them: world to view, view to clip (unjittered, rows flipped
    // so NRD's first image row is ours: the bottom), this frame's and last frame's
    float view[16] = {}, proj[16] = {}, view_prev[16] = {}, proj_prev[16] = {};
    float sun[3] = {};          // toward the sun, world space
    float ao_range = 1.5f;      // the AO rays' reach (its hit distances normalized by it)
    bool sun_shadow = false, ao = false, lights = false, gi = false;  // the denoisers to run
    bool reset = false;         // the history starts over (a camera cut, NRD off last frame)
    float denoising_range = 100000.0f;
};

// The images at this size (made again on a change, the history then started over),
// their layouts set in `cmd` the first time; false: NRD isn't available
bool Prepare(VkCommandBuffer cmd, uint32_t width, uint32_t height);
VkImageView View(Image image);
// The frame index the next Denoise hands NRD (GI's checkerboard: the cells traced, BLACK
// mode, are where (x + y + it) is even)
uint32_t FrameIndex();

// The denoisers' passes recorded into `cmd`, after PREPARE wrote the inputs (barriers
// before, between and after: the outputs ready for compute reads). `ring` (0..3): which
// part of the constants buffer, one the GPU is done with
bool Denoise(VkCommandBuffer cmd, const Frame& frame, int ring);

}  // namespace VkNrd
