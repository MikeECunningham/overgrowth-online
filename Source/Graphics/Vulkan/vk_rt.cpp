//-----------------------------------------------------------------------------
//           Name: vk_rt.cpp
//      Developer: Overgrowth Online (crxhvrd)
//    Description: See vk_rt.h
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#include <volk.h>
#include <vk_mem_alloc.h>

#include "vk_rt.h"

#include <Graphics/Vulkan/vk_backend.h>
#include <Graphics/Vulkan/vk_compile.h>
#include <Graphics/Vulkan/vk_gl.h>
#include <Graphics/Vulkan/vk_nrd.h>
#include <Graphics/model.h>
#include <Internal/filesystem.h>
#include <Internal/profiler.h>
#include <Internal/textfile.h>
#include <Logging/logdata.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace VkRt {
namespace {

// Meshes not drawn for this many scenes give their BLAS back
const uint64_t kForgetAfterScenes = 600;
// BLAS builds' scratch in one scene at most (a level's first frame builds the rest over
// the next ones; their meshes aren't traced until then)
const VkDeviceSize kScratchPerScene = 256ull << 20;
const int kTlasSets = 4;  // (each scene its own: a frame may draw several, two frames in flight)
// The textures alpha-tested hits read (a descriptor array: each colour texture its slot,
// a new slot when its image is made again; slots are never written twice)
const uint32_t kTextureSlots = 4096;
// Static BLASes waiting for their compacted size at once at most (more: left as built)
const uint32_t kCompactQueries = 1024;
// A skinned BLAS is refitted each scene, built again after this many (refits loosen it)
const int kRefitsBeforeRebuild = 60;
// The models' front faces as the colour pass culls them (GL's counter-clockwise, in a
// right-handed world: cross(v1 - v0, v2 - v0) points out of the front) are already ray
// tracing's by default (a ray going against that cross product hits a front face), so
// no flip: the default facing, culling allowed
const VkGeometryInstanceFlagsKHR kGlFacing = 0;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = NULL;
    VkDeviceAddress address = 0;
    void* mapped = NULL;
    VkDeviceSize size = 0;
};

struct Blas {
    std::string path;
    size_t vertices = 0, faces = 0;  // (the model's when it was made: one that changed is made again)
    // the positions (3 floats each), the faces' indices, the uvs (2 floats each), the aux (3
    // floats each): in VRAM (hits read the indices and uvs ray by ray), copied there from
    // `staging` before the build
    Buffer geometry;
    Buffer staging;
    VkDeviceSize index_offset = 0, uv_offset = 0, aux_offset = 0;
    Buffer storage;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize scratch_size = 0;
    bool built = false;
    // Compaction: the compacted size's query slot (-1: none), the commands that wrote it
    int compact_query = -1;
    uint64_t compact_serial = 0;
    bool compacted = false;
    uint64_t last_scene = 0;
};

// An instance's material (rt_shadow.comp's and rt_reflect.glsl's; every instance's
// instanceCustomIndex): where a hit's uvs are, its colour texture, its tint
struct Material {
    VkDeviceAddress indices;  // the faces' indices
    VkDeviceAddress uvs;      // its vertices' uvs (2 floats each; 0: none)
    uint32_t texture;         // the texture slot (kNoTexture: none)
    float scroll;             // how far its texture moves down a second (#SCROLL_*)
    uint32_t flags;           // kMaterial*
    uint32_t tint_mask;       // the slot of what masks its tint by its alpha (kNoTexture: all of it)
    float tint[3];            // its colour tint
    float emission;           // an emitter's light, by its colour (Surface::emission)
};
static_assert(sizeof(Material) == 48, "rt_shadow.comp's and rt_reflect.glsl's Material");
const uint32_t kNoTexture = 0xFFFFFFFFu;
const uint32_t kMaterialAlphaTested = 1u, kMaterialEmitter = 2u, kMaterialWaterfall = 4u, kMaterialGrass = 8u;

// A character's mesh: its bind pose and skinning data (static), its positions skinned
// each scene, its BLAS refitted from them
struct SkinnedBlas {
    std::string path;
    size_t vertices = 0, faces = 0;
    Buffer input;  // the bind pose (3 floats each), bone ids and weights (4 floats each), the faces' indices, the uvs (2 floats each): VRAM
    Buffer staging;  // (input's, till it's copied at the first skinning)
    VkDeviceSize ids_offset = 0, weights_offset = 0, index_offset = 0, uv_offset = 0;
    Buffer positions;  // skinned, world space (3 floats each)
    Buffer storage;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize build_scratch = 0, update_scratch = 0;
    bool built = false;
    int refits = 0;
    int stagger = 0;  // (where in the cycle its first build starts it: not every character rebuilt in the same scene)
    uint64_t last_scene = 0;
};

// This scene's skinning of one: where its bones and morphs went in the upload
struct Skin {
    SkinnedBlas* blas;
    VkDeviceSize bones_offset, morphs_offset;  // (morphs: ~0 none)
};

// rt_skin.comp's push constants
struct SkinPush {
    VkDeviceAddress positions, bone_ids, weights, morphs, bones, out_positions;
    uint32_t vertices, has_morphs;
};

// A plant near the camera: its vertices moved as its draw moves them each scene (bend,
// wind, a push's shake), its own BLAS refitted from them; its model's shared Blas holds
// the rest (the bind pose, aux, indices, uvs)
struct PlantBlas {
    const Model* model = NULL;
    const void* source = NULL;  // (the shared Blas it was made from: one made again is followed)
    size_t vertices = 0, faces = 0;
    Buffer positions;  // world space (3 floats each)
    Buffer storage;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize build_scratch = 0, update_scratch = 0;
    bool built = false;
    int refits = 0;
    int stagger = 0;
    uint64_t last_scene = 0;
};

// rt_plant.comp's push constants
struct PlantPush {
    float scale[4];        // xyz, w the time
    float rotation[4];     // a quaternion
    float translation[4];  // xyz, w the shake
    VkDeviceAddress positions, aux, out_positions;
    uint32_t vertices, waterfall;
};

struct Tlas {
    Buffer instances, storage, scratch;
    Buffer upload;  // the scene's bones and morphs
    VkDeviceSize upload_capacity = 0;
    Buffer skin_scratch;  // the skinned BLASes' builds and refits
    VkDeviceSize skin_scratch_capacity = 0;
    Buffer plant_scratch;  // the moving plants' builds and refits
    VkDeviceSize plant_scratch_capacity = 0;
    Buffer materials;  // the scene's Materials
    uint32_t materials_capacity = 0;
    Buffer lights;  // the scene's point lights (LightingPass::lights)
    uint32_t lights_capacity = 0;
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    uint32_t capacity = 0;  // instances
    uint64_t serial = 0;    // the commands that last used it
};

struct State {
    bool tried = false;
    bool available = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout textures_layout = VK_NULL_HANDLE;  // (set 1: the texture slots)
    VkDescriptorPool textures_pool = VK_NULL_HANDLE;
    VkDescriptorSet textures_set = VK_NULL_HANDLE;
    VkSampler texture_sampler = VK_NULL_HANDLE;
    // A texture's slot: what its image was when it got it (the handles alone can come back
    // for another image once the first is gone: its size and format must match too), and
    // the scene that last used it. Slots not used for a while, or whose texture's image was
    // made again, go back to the free list with the serial of the last commands that could
    // read them, and are written again only once those are done
    struct TextureSlotInfo {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0, levels = 0;
        uint32_t slot = 0;
        uint64_t last_scene = 0;
    };
    std::unordered_map<GLuint, TextureSlotInfo> texture_slots;
    struct FreeSlot {
        uint32_t slot;
        uint64_t serial;
    };
    std::vector<FreeSlot> free_slots;
    uint32_t next_slot = 0;
    std::vector<Material> materials;   // the scene's (one an instance)
    int alpha_skipped = 0;             // (the scene's alpha-tested meshes left out: their texture isn't there yet)
    // The scene handed to draws (SceneForDraws), from its trace till EndScene, and its TLAS
    // set (its serial follows the draws: the materials the CPU writes again when the set
    // comes round must wait for them, not just for the build)
    bool draw_scene = false;
    DrawScene drawn;
    struct Tlas* drawn_set = NULL;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline shadow[2] = {};  // the depth single-sampled, multisampled
    VkDescriptorSetLayout denoise_set_layout = VK_NULL_HANDLE;  // (six storage images, pushed)
    VkPipelineLayout denoise_layout = VK_NULL_HANDLE;
    VkPipeline denoise[2] = {};  // temporal, spatial
    VkPipeline gi_denoise = VK_NULL_HANDLE;
    // NRD's (rt_nrd.comp): its inputs made, its outputs into the mask (made the first time
    // NRD is asked for)
    bool nrd_tried = false;
    uint32_t nrd_dispatches = 0;
    VkDescriptorSetLayout nrd_set_layout[2] = {};  // prepare, composite (storage images, pushed)
    VkPipelineLayout nrd_layout[2] = {};
    VkPipeline nrd[2] = {};
    // Global illumination's (rt_gi.comp, rt_cache.comp): made the first time it's asked for
    bool gi_tried = false;
    VkDescriptorSetLayout gi_set_layout = VK_NULL_HANDLE, cache_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout gi_layout = VK_NULL_HANDLE, cache_layout = VK_NULL_HANDLE;
    VkPipeline gi[2] = {};  // the depth single-sampled, multisampled
    VkPipeline cache = VK_NULL_HANDLE;
    VkSampler sky_sampler = VK_NULL_HANDLE;
    Buffer cache_keys, cache_accum, cache_values, cache_ages;  // (the radiance cache: kCacheVoxels each)
    bool cache_fresh = true;  // (made, not cleared yet)
    // The level's light probes as a grid (LightingPass::probe_grid) in VRAM, its version;
    // its staging (kept till the next version: the copy may still be in flight)
    Buffer probes, probes_staging;
    uint64_t probes_version = ~0ull;
    bool gi_drawn = false;    // (this scene's GI is in LightingPass::gi_out)
    VkSampler sampler = VK_NULL_HANDLE;
    VkDeviceSize scratch_align = 256;
    std::unordered_map<const Model*, Blas> blases;
    std::vector<const Model*> pending;  // made, not built yet
    // The static BLASes' compacted sizes, a query each, read back once their builds are done
    VkQueryPool compact_pool = VK_NULL_HANDLE;
    std::vector<int> free_queries;
    std::vector<const Model*> compacting;  // (built, their query written, not compacted yet)
    std::map<std::pair<const void*, const Model*>, SkinnedBlas> skinned;
    std::vector<Skin> skins;  // the scene's
    std::vector<char> upload;  // (the scene's bones and morphs, gathered)
    VkPipelineLayout skin_layout = VK_NULL_HANDLE;
    VkPipeline skin = VK_NULL_HANDLE;
    std::map<const void*, PlantBlas> plants;  // (by the plant's object)
    struct PlantWork {
        PlantBlas* blas;
        const Blas* source;
        PlantPush push;
    };
    std::vector<PlantWork> plant_work;  // the scene's
    VkPipelineLayout plant_layout = VK_NULL_HANDLE;
    VkPipeline plant = VK_NULL_HANDLE;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<const bool*> instance_built;  // (each instance's BLAS built: those not built yet are left out)
    uint64_t scene = 0;
    Tlas tlas[kTlasSets];
    int next_tlas = 0;
};
State s;

// The shadow pass's push constants (rt_shadow.comp's Params)
struct ShadowPush {
    float inv_view_proj[16];
    float sun[4];        // xyz toward the sun, w its angular radius
    uint32_t info[4];    // width, height, frame, sun rays | AO rays << 8 | light rays << 16 | the lights' clearance (cm) << 24
    float camera[4];     // xyz the camera, w the ray's start off the surface per unit of distance
    float ao[4];         // x the AO's range, y the time (the draw's: scrolling textures), z the lights' count, w their radius
};

// rt_gi.comp's push constants
struct GiPush {
    float inv_view_proj[16];
    float camera[4];     // xyz, w the ray's start off the surface per unit of distance
    float sun[4];        // xyz toward the sun, w how much of the point lights' light bounces
    float sun_color[4];  // rgb, w the time
    uint32_t info[4];    // width, height, frame, the point lights' count
    float params[4];     // the cache's smallest voxel, a glow's strength, the sky's lod, the sky's strength
    uint32_t debug[4];   // x LightingPass::gi_debug, y the checkerboard's phase (VkNrd::FrameIndex() & 1), z 1: checkerboard
    float look[4];       // x the strength of the light a pixel receives (rt_gi_strength; not the cache's), y the colour pass's GetAmbientContrib(1)
};

// The radiance cache's voxels (a power of two: rt_gi.comp masks with it)
const uint32_t kCacheVoxels = 1u << 20;

// rt_nrd.comp #PREPARE's push constants
struct NrdPreparePush {
    float inv_view_proj[16];  // as drawn (jittered)
    float cur[16];            // world to this frame's clip, unjittered
    float prev[16];           // to last frame's
    float params[4];          // tan(the sun's angular radius), the sky's view Z
    uint32_t info[4];         // width, height, signals (1 sun, 2 AO, 4 lights, 8 GI), GI's checkerboard phase
};

// rt_denoise.comp's push constants
struct DenoisePush {
    float reprojection[16];
    uint32_t info[4];   // width, height, frame, flags (1 history, 2 denoise)
    float params[4];    // the least share of a new frame, the most frames kept, the near plane, a pixel's angle
    float extra[4];     // the sun's diameter as a slope, the AO's range
};

VkDeviceSize AlignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer* out) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 16);
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo a = {};
    a.usage = VMA_MEMORY_USAGE_AUTO;
    if (host) {
        a.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo ai = {};
    if (vmaCreateBuffer(VkBackend::Allocator(), &info, &a, &out->buffer, &out->alloc, &ai) != VK_SUCCESS) {
        *out = Buffer();
        return false;
    }
    out->mapped = ai.pMappedData;
    out->size = info.size;
    VkBufferDeviceAddressInfo da = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = out->buffer;
    out->address = vkGetBufferDeviceAddress(VkBackend::Device(), &da);
    return true;
}

// Once the GPU is done with it (now: at shutdown)
void FreeBuffer(Buffer* b, bool now = false) {
    if (b->buffer != VK_NULL_HANDLE) {
        VkBuffer buffer = b->buffer;
        VmaAllocation alloc = b->alloc;
        if (now) {
            vmaDestroyBuffer(VkBackend::Allocator(), buffer, alloc);
        } else {
            VkBackend::DeferDestroy([buffer, alloc]() { vmaDestroyBuffer(VkBackend::Allocator(), buffer, alloc); });
        }
    }
    *b = Buffer();
}

void FreeAs(VkAccelerationStructureKHR* as, bool now = false) {
    if (*as != VK_NULL_HANDLE) {
        VkAccelerationStructureKHR a = *as;
        if (now) {
            vkDestroyAccelerationStructureKHR(VkBackend::Device(), a, NULL);
        } else {
            VkBackend::DeferDestroy([a]() { vkDestroyAccelerationStructureKHR(VkBackend::Device(), a, NULL); });
        }
    }
    *as = VK_NULL_HANDLE;
}

void FreeBlas(Blas* b, bool now = false) {
    FreeAs(&b->as, now);
    FreeBuffer(&b->storage, now);
    FreeBuffer(&b->geometry, now);
    FreeBuffer(&b->staging, now);
    if (b->compact_query >= 0) {
        s.free_queries.push_back(b->compact_query);
    }
    *b = Blas();
}

void FreeTlas(Tlas* t, bool now = false) {
    FreeAs(&t->as, now);
    FreeBuffer(&t->materials, now);
    t->materials_capacity = 0;
    FreeBuffer(&t->lights, now);
    t->lights_capacity = 0;
    FreeBuffer(&t->upload, now);
    t->upload_capacity = 0;
    FreeBuffer(&t->skin_scratch, now);
    t->skin_scratch_capacity = 0;
    FreeBuffer(&t->plant_scratch, now);
    t->plant_scratch_capacity = 0;
    FreeBuffer(&t->instances, now);
    FreeBuffer(&t->storage, now);
    FreeBuffer(&t->scratch, now);
    t->capacity = 0;
}

void FreeSkinned(SkinnedBlas* b, bool now = false) {
    FreeAs(&b->as, now);
    FreeBuffer(&b->storage, now);
    FreeBuffer(&b->positions, now);
    FreeBuffer(&b->input, now);
    FreeBuffer(&b->staging, now);
    *b = SkinnedBlas();
}

void FreePlant(PlantBlas* b, bool now = false) {
    FreeAs(&b->as, now);
    FreeBuffer(&b->storage, now);
    FreeBuffer(&b->positions, now);
    *b = PlantBlas();
}

void Shutdown() {
    for (auto& b : s.plants) {
        FreePlant(&b.second, true);
    }
    s.plants.clear();
    s.plant_work.clear();
    if (s.plant != VK_NULL_HANDLE) {
        vkDestroyPipeline(VkBackend::Device(), s.plant, NULL);
        s.plant = VK_NULL_HANDLE;
    }
    if (s.plant_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(VkBackend::Device(), s.plant_layout, NULL);
        s.plant_layout = VK_NULL_HANDLE;
    }
    for (auto& b : s.blases) {
        FreeBlas(&b.second, true);
    }
    s.blases.clear();
    s.compacting.clear();
    s.free_queries.clear();
    if (s.compact_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(VkBackend::Device(), s.compact_pool, NULL);
        s.compact_pool = VK_NULL_HANDLE;
    }
    for (auto& b : s.skinned) {
        FreeSkinned(&b.second, true);
    }
    s.skinned.clear();
    s.skins.clear();
    if (s.skin != VK_NULL_HANDLE) {
        vkDestroyPipeline(VkBackend::Device(), s.skin, NULL);
        s.skin = VK_NULL_HANDLE;
    }
    if (s.skin_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(VkBackend::Device(), s.skin_layout, NULL);
        s.skin_layout = VK_NULL_HANDLE;
    }
    s.pending.clear();
    for (Tlas& t : s.tlas) {
        FreeTlas(&t, true);
        t.serial = 0;
    }
    VkDevice dev = VkBackend::Device();
    for (VkPipeline& p : s.shadow) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, p, NULL);
            p = VK_NULL_HANDLE;
        }
    }
    for (VkPipeline& p : s.denoise) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, p, NULL);
            p = VK_NULL_HANDLE;
        }
    }
    if (s.denoise_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, s.denoise_layout, NULL);
        s.denoise_layout = VK_NULL_HANDLE;
    }
    if (s.denoise_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, s.denoise_set_layout, NULL);
        s.denoise_set_layout = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 2; ++i) {
        if (s.nrd[i] != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, s.nrd[i], NULL);
            s.nrd[i] = VK_NULL_HANDLE;
        }
        if (s.nrd_layout[i] != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, s.nrd_layout[i], NULL);
            s.nrd_layout[i] = VK_NULL_HANDLE;
        }
        if (s.nrd_set_layout[i] != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, s.nrd_set_layout[i], NULL);
            s.nrd_set_layout[i] = VK_NULL_HANDLE;
        }
    }
    s.nrd_tried = false;
    for (VkPipeline* pl : {&s.gi[0], &s.gi[1], &s.cache, &s.gi_denoise}) {
        if (*pl != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, *pl, NULL);
            *pl = VK_NULL_HANDLE;
        }
    }
    for (VkPipelineLayout* pl : {&s.gi_layout, &s.cache_layout}) {
        if (*pl != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, *pl, NULL);
            *pl = VK_NULL_HANDLE;
        }
    }
    for (VkDescriptorSetLayout* sl : {&s.gi_set_layout, &s.cache_set_layout}) {
        if (*sl != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, *sl, NULL);
            *sl = VK_NULL_HANDLE;
        }
    }
    if (s.sky_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, s.sky_sampler, NULL);
        s.sky_sampler = VK_NULL_HANDLE;
    }
    FreeBuffer(&s.cache_keys, true);
    FreeBuffer(&s.cache_accum, true);
    FreeBuffer(&s.cache_values, true);
    FreeBuffer(&s.cache_ages, true);
    FreeBuffer(&s.probes, true);
    FreeBuffer(&s.probes_staging, true);
    s.probes_version = ~0ull;
    s.cache_fresh = true;
    s.gi_tried = false;
    if (s.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, s.layout, NULL);
        s.layout = VK_NULL_HANDLE;
    }
    if (s.set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, s.set_layout, NULL);
        s.set_layout = VK_NULL_HANDLE;
    }
    if (s.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, s.sampler, NULL);
        s.sampler = VK_NULL_HANDLE;
    }
    if (s.texture_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, s.texture_sampler, NULL);
        s.texture_sampler = VK_NULL_HANDLE;
    }
    if (s.textures_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, s.textures_pool, NULL);  // (its set with it)
        s.textures_pool = VK_NULL_HANDLE;
        s.textures_set = VK_NULL_HANDLE;
    }
    if (s.textures_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, s.textures_layout, NULL);
        s.textures_layout = VK_NULL_HANDLE;
    }
    s.texture_slots.clear();
    s.free_slots.clear();
    s.next_slot = 0;
    s.materials.clear();
    s.draw_scene = false;
    s.drawn = DrawScene();
    s.drawn_set = NULL;
    s.available = false;
    s.tried = false;  // (a renderer started again asks again)
}

// rt_shadow.comp with `defines` after its #version line, as a pipeline
VkPipeline MakeComputePipeline(const std::string& source, const char* defines, const char* name, VkPipelineLayout layout) {
    std::string text = source;
    const size_t version = text.find("#version");
    const size_t line_end = version == std::string::npos ? std::string::npos : text.find('\n', version);
    if (line_end != std::string::npos) {
        text.insert(line_end + 1, defines);
    }
    std::vector<uint32_t> spirv;
    std::string error;
    if (!VkCompile::CompileCompute(text, &spirv, &error)) {
        LOGE << "Ray tracing: " << name << " doesn't compile: " << error << std::endl;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * sizeof(uint32_t);
    mi.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(VkBackend::Device(), &mi, NULL, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo pi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = module;
    pi.stage.pName = "main";
    pi.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(VkBackend::Device(), VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) != VK_SUCCESS) {
        pipeline = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(VkBackend::Device(), module, NULL);
    return pipeline;
}

bool Start() {
    s.tried = true;
    if (!VkBackend::RayQuery()) {
        LOGI << "Ray tracing: the device has no ray queries (or vk_no_ray_tracing)" << std::endl;
        return false;
    }
    VkDevice dev = VkBackend::Device();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(VkBackend::PhysicalDevice(), &p2);
    s.scratch_align = std::max<VkDeviceSize>(asp.minAccelerationStructureScratchOffsetAlignment, 1);
    // (the static BLASes' compacted sizes: without one, they're left as built)
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qpi.queryCount = kCompactQueries;
    if (vkCreateQueryPool(dev, &qpi, NULL, &s.compact_pool) == VK_SUCCESS) {
        for (int i = (int)kCompactQueries - 1; i >= 0; --i) {
            s.free_queries.push_back(i);
        }
    } else {
        s.compact_pool = VK_NULL_HANDLE;
    }

    const int kPathSize = 1024;
    char path[kPathSize];
    std::string source;
    if (FindFilePath("Data/GLSL/rt_shadow.comp", path, kPathSize, kDataPaths | kModPaths, false) == -1) {
        LOGE << "Ray tracing: no Data/GLSL/rt_shadow.comp" << std::endl;
        return false;
    }
    textFileRead(path, &source);

    // Set 0, pushed: the scene, the depth, the raw result, the alpha-tested instances, the
    // penumbras' sizes, the point lights
    VkDescriptorSetLayoutBinding b[6] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[4].binding = 4;
    b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[5].binding = 5;
    b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (VkDescriptorSetLayoutBinding& x : b) {
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = 6;
    li.pBindings = b;
    // Set 1: the texture slots (written as textures get one, while earlier frames run)
    VkDescriptorSetLayoutBinding tb = {};
    tb.binding = 0;
    tb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tb.descriptorCount = kTextureSlots;
    tb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;  // (and the colour pass's reflections)
    const VkDescriptorBindingFlags tb_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo tfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    tfi.bindingCount = 1;
    tfi.pBindingFlags = &tb_flags;
    VkDescriptorSetLayoutCreateInfo tli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tli.pNext = &tfi;
    tli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    tli.bindingCount = 1;
    tli.pBindings = &tb;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTextureSlots};
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorSetLayout(dev, &tli, NULL, &s.textures_layout) != VK_SUCCESS ||
        vkCreateDescriptorPool(dev, &pool_info, NULL, &s.textures_pool) != VK_SUCCESS) {
        LOGE << "Ray tracing: couldn't make the texture slots" << std::endl;
        Shutdown();
        s.tried = true;
        return false;
    }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.textures_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s.textures_layout;
    if (vkAllocateDescriptorSets(dev, &ai, &s.textures_set) != VK_SUCCESS) {
        LOGE << "Ray tracing: couldn't make the texture slots" << std::endl;
        Shutdown();
        s.tried = true;
        return false;
    }
    VkSamplerCreateInfo tsi = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    tsi.magFilter = VK_FILTER_LINEAR;
    tsi.minFilter = VK_FILTER_LINEAR;
    tsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    tsi.addressModeU = tsi.addressModeV = tsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    tsi.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &tsi, NULL, &s.texture_sampler) != VK_SUCCESS) {
        Shutdown();
        s.tried = true;
        return false;
    }
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShadowPush)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateDescriptorSetLayout(dev, &li, NULL, &s.set_layout) != VK_SUCCESS) {
        Shutdown();
        s.tried = true;
        return false;
    }
    const VkDescriptorSetLayout both[2] = {s.set_layout, s.textures_layout};
    pli.pSetLayouts = both;
    if (vkCreatePipelineLayout(dev, &pli, NULL, &s.layout) != VK_SUCCESS || vkCreateSampler(dev, &si, NULL, &s.sampler) != VK_SUCCESS) {
        LOGE << "Ray tracing: couldn't make the pass's layout" << std::endl;
        Shutdown();
        s.tried = true;
        return false;
    }
    s.shadow[0] = MakeComputePipeline(source, "", "rt_shadow.comp", s.layout);
    s.shadow[1] = MakeComputePipeline(source, "#define MSAA_DEPTH 1\n", "rt_shadow.comp #MSAA_DEPTH", s.layout);
    // The skinning: push constants only (every buffer by address)
    std::string skin_source;
    if (FindFilePath("Data/GLSL/rt_skin.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &skin_source);
    }
    VkPushConstantRange skin_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinPush)};
    VkPipelineLayoutCreateInfo skin_li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    skin_li.pushConstantRangeCount = 1;
    skin_li.pPushConstantRanges = &skin_range;
    if (!skin_source.empty() && vkCreatePipelineLayout(dev, &skin_li, NULL, &s.skin_layout) == VK_SUCCESS) {
        s.skin = MakeComputePipeline(skin_source, "", "rt_skin.comp", s.skin_layout);
    }
    if (s.skin == VK_NULL_HANDLE) {
        LOGW << "Ray tracing: no rt_skin.comp: characters are left out" << std::endl;
    }
    // The moving plants: push constants only too
    std::string plant_source;
    if (FindFilePath("Data/GLSL/rt_plant.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &plant_source);
    }
    VkPushConstantRange plant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PlantPush)};
    VkPipelineLayoutCreateInfo plant_li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plant_li.pushConstantRangeCount = 1;
    plant_li.pPushConstantRanges = &plant_range;
    if (!plant_source.empty() && vkCreatePipelineLayout(dev, &plant_li, NULL, &s.plant_layout) == VK_SUCCESS) {
        s.plant = MakeComputePipeline(plant_source, "", "rt_plant.comp", s.plant_layout);
    }
    if (s.plant == VK_NULL_HANDLE) {
        LOGW << "Ray tracing: no rt_plant.comp: plants are traced where they stand, without the wind" << std::endl;
    }
    // The denoiser: six storage images, pushed (raw, last frame's raw, history in and
    // out, the mask, the penumbras' sizes)
    std::string denoise_source;
    if (FindFilePath("Data/GLSL/rt_denoise.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &denoise_source);
    }
    VkDescriptorSetLayoutBinding db[6] = {};
    for (int i = 0; i < 6; ++i) {
        db[i].binding = (uint32_t)i;
        db[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        db[i].descriptorCount = 1;
        db[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dli.bindingCount = 6;
    dli.pBindings = db;
    VkPushConstantRange denoise_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DenoisePush)};
    VkPipelineLayoutCreateInfo denoise_li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    denoise_li.setLayoutCount = 1;
    denoise_li.pSetLayouts = &s.denoise_set_layout;
    denoise_li.pushConstantRangeCount = 1;
    denoise_li.pPushConstantRanges = &denoise_range;
    if (!denoise_source.empty() && vkCreateDescriptorSetLayout(dev, &dli, NULL, &s.denoise_set_layout) == VK_SUCCESS &&
        vkCreatePipelineLayout(dev, &denoise_li, NULL, &s.denoise_layout) == VK_SUCCESS) {
        s.denoise[0] = MakeComputePipeline(denoise_source, "#define TEMPORAL 1\n", "rt_denoise.comp #TEMPORAL", s.denoise_layout);
        s.denoise[1] = MakeComputePipeline(denoise_source, "#define SPATIAL 1\n", "rt_denoise.comp #SPATIAL", s.denoise_layout);
    }
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        VkBackend::OnShutdown(&Shutdown);
    }
    if (s.shadow[0] == VK_NULL_HANDLE || s.shadow[1] == VK_NULL_HANDLE || s.denoise[0] == VK_NULL_HANDLE || s.denoise[1] == VK_NULL_HANDLE) {
        Shutdown();
        s.tried = true;
        return false;
    }
    s.available = true;
    LOGI << "Ray tracing: ray queries on (BLAS scratch alignment " << s.scratch_align << ")" << std::endl;
    return true;
}

VkAccelerationStructureGeometryKHR TriangleGeometry(const Blas& b) {
    VkAccelerationStructureGeometryKHR g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    g.geometry.triangles.vertexData.deviceAddress = b.geometry.address;
    g.geometry.triangles.vertexStride = 3 * sizeof(float);
    g.geometry.triangles.maxVertex = (uint32_t)b.vertices - 1;
    g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    g.geometry.triangles.indexData.deviceAddress = b.geometry.address + b.index_offset;
    g.flags = 0;  // (opaque or not: each instance says)
    return g;
}

// A BLAS's triangles readable by ray queries (a reflection's hit), where the device can
VkBuildAccelerationStructureFlagsKHR DataAccess() {
    return VkBackend::RtPositionFetch() ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DATA_ACCESS_BIT_KHR : 0;
}

VkAccelerationStructureBuildGeometryInfoKHR BlasBuildInfo(const VkAccelerationStructureGeometryKHR* g) {
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR | DataAccess();
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = g;
    return info;
}

// The model's geometry copied, its BLAS made (built at the next trace)
bool MakeBlas(const Model& m, Blas* b) {
    b->vertices = m.vertices.size() / 3;
    b->faces = m.faces.size() / 3;
    b->path = m.path;
    const VkDeviceSize vertex_bytes = b->vertices * 3 * sizeof(float);
    const VkDeviceSize index_bytes = b->faces * 3 * sizeof(uint32_t);
    const VkDeviceSize uv_bytes = b->vertices * 2 * sizeof(float);
    b->index_offset = AlignUp(vertex_bytes, 16);
    b->uv_offset = AlignUp(b->index_offset + index_bytes, 16);
    b->aux_offset = AlignUp(b->uv_offset + uv_bytes, 16);
    const VkDeviceSize total_bytes = b->aux_offset + vertex_bytes;
    if (!MakeBuffer(total_bytes,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    false, &b->geometry) ||
        !MakeBuffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &b->staging) || b->staging.mapped == NULL) {
        FreeBlas(b);
        return false;
    }
    char* dst = (char*)b->staging.mapped;
    memcpy(dst, m.vertices.data(), vertex_bytes);
    memcpy(dst + b->index_offset, m.faces.data(), index_bytes);
    if (m.tex_coords.size() >= b->vertices * 2) {
        memcpy(dst + b->uv_offset, m.tex_coords.data(), uv_bytes);
    } else {
        memset(dst + b->uv_offset, 0, uv_bytes);
    }
    // (a plant's stability per vertex, its first of three: rt_plant.comp)
    if (m.aux.size() >= b->vertices * 3) {
        memcpy(dst + b->aux_offset, m.aux.data(), vertex_bytes);
    } else {
        memset(dst + b->aux_offset, 0, vertex_bytes);
    }
    vmaFlushAllocation(VkBackend::Allocator(), b->staging.alloc, 0, VK_WHOLE_SIZE);

    const VkAccelerationStructureGeometryKHR g = TriangleGeometry(*b);
    const VkAccelerationStructureBuildGeometryInfoKHR info = BlasBuildInfo(&g);
    const uint32_t primitives = (uint32_t)b->faces;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(VkBackend::Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
    if (!MakeBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, &b->storage)) {
        FreeBlas(b);
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = b->storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(VkBackend::Device(), &ci, NULL, &b->as) != VK_SUCCESS) {
        b->as = VK_NULL_HANDLE;
        FreeBlas(b);
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = b->as;
    b->address = vkGetAccelerationStructureDeviceAddressKHR(VkBackend::Device(), &ai);
    b->scratch_size = sizes.buildScratchSize;
    b->built = false;
    return true;
}

void AsBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dst_stage) {
    VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    m.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    m.dstStageMask = dst_stage;
    m.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Copies into VRAM (the geometry from its staging) done before what reads them: the
// builds, the compute passes (skinning, plants, our traces) and the colour pass's hits
void UploadBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    m.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    m.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

// The pending BLASes built, as many as the scene's scratch allows
void BuildPending(VkCommandBuffer cmd) {
    if (s.pending.empty()) {
        return;
    }
    std::vector<Blas*> build;
    std::vector<const Model*> build_models;
    std::vector<const Model*> later;
    VkDeviceSize scratch_total = 0;
    for (const Model* m : s.pending) {
        auto it = s.blases.find(m);
        if (it == s.blases.end() || it->second.as == VK_NULL_HANDLE || it->second.built) {
            continue;
        }
        const VkDeviceSize need = AlignUp(it->second.scratch_size, s.scratch_align);
        if (!build.empty() && scratch_total + need > kScratchPerScene) {
            later.push_back(m);
            continue;
        }
        build.push_back(&it->second);
        build_models.push_back(m);
        scratch_total += need;
    }
    s.pending.swap(later);
    if (build.empty()) {
        return;
    }
    Buffer scratch;
    if (!MakeBuffer(scratch_total + s.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &scratch)) {
        LOGE << "Ray tracing: no memory for " << build.size() << " BLAS builds" << std::endl;
        return;
    }
    // Their geometry into VRAM first (the build, the plants' moves and the hits read it)
    for (Blas* b : build) {
        if (b->staging.buffer != VK_NULL_HANDLE) {
            VkBufferCopy copy = {0, 0, b->geometry.size};
            vkCmdCopyBuffer(cmd, b->staging.buffer, b->geometry.buffer, 1, &copy);
            FreeBuffer(&b->staging);  // (once these commands are done)
        }
    }
    UploadBarrier(cmd);
    const VkDeviceAddress base = AlignUp(scratch.address, s.scratch_align);
    std::vector<VkAccelerationStructureGeometryKHR> geometries(build.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(build.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(build.size());
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_ptrs(build.size());
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < build.size(); ++i) {
        geometries[i] = TriangleGeometry(*build[i]);
        infos[i] = BlasBuildInfo(&geometries[i]);
        infos[i].dstAccelerationStructure = build[i]->as;
        infos[i].scratchData.deviceAddress = base + offset;
        offset += AlignUp(build[i]->scratch_size, s.scratch_align);
        ranges[i] = {};
        ranges[i].primitiveCount = (uint32_t)build[i]->faces;
        range_ptrs[i] = &ranges[i];
        build[i]->built = true;
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), range_ptrs.data());
    FreeBuffer(&scratch);  // (once these commands are done)

    // Their compacted sizes asked for (read back once these commands are done: CompactBuilt)
    if (s.compact_pool != VK_NULL_HANDLE) {
        AsBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
        for (size_t i = 0; i < build.size(); ++i) {
            if (s.free_queries.empty()) {
                break;
            }
            Blas* b = build[i];
            b->compact_query = s.free_queries.back();
            s.free_queries.pop_back();
            b->compact_serial = VkBackend::RecordingSerial();
            vkCmdResetQueryPool(cmd, s.compact_pool, (uint32_t)b->compact_query, 1);
            vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &b->as, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, s.compact_pool,
                                                          (uint32_t)b->compact_query);
            s.compacting.push_back(build_models[i]);
        }
    }
}

// The static BLASes built a while ago made again at their compacted size (often under
// half): the copy recorded now, the old one freed once what reads it is done. `remap`:
// the scene's instances already gathered are pointed at the new one
void CompactBuilt(VkCommandBuffer cmd) {
    if (s.compacting.empty() || s.compact_pool == VK_NULL_HANDLE) {
        return;
    }
    const uint64_t completed = VkBackend::CompletedSerial();
    std::vector<const Model*> still;
    std::unordered_map<VkDeviceAddress, VkDeviceAddress> moved;
    bool copied = false;
    for (const Model* m : s.compacting) {
        auto it = s.blases.find(m);
        if (it == s.blases.end() || it->second.compact_query < 0 || it->second.compacted) {
            continue;  // (forgotten or made again meanwhile)
        }
        Blas& b = it->second;
        if (b.compact_serial > completed) {
            still.push_back(m);
            continue;
        }
        uint64_t size = 0;
        const VkResult r = vkGetQueryPoolResults(VkBackend::Device(), s.compact_pool, (uint32_t)b.compact_query, 1, sizeof(size), &size, sizeof(size),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r == VK_NOT_READY) {
            still.push_back(m);
            continue;
        }
        s.free_queries.push_back(b.compact_query);
        b.compact_query = -1;
        b.compacted = true;  // (tried: not again)
        if (r != VK_SUCCESS || size == 0 || size >= b.storage.size) {
            continue;
        }
        Buffer storage;
        if (!MakeBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, &storage)) {
            continue;
        }
        VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = storage.buffer;
        ci.size = size;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        if (vkCreateAccelerationStructureKHR(VkBackend::Device(), &ci, NULL, &as) != VK_SUCCESS) {
            FreeBuffer(&storage);
            continue;
        }
        VkCopyAccelerationStructureInfoKHR copy = {VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copy.src = b.as;
        copy.dst = as;
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd, &copy);
        copied = true;
        VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = as;
        const VkDeviceAddress address = vkGetAccelerationStructureDeviceAddressKHR(VkBackend::Device(), &ai);
        moved[b.address] = address;
        FreeAs(&b.as);  // (once these commands are done: last frames' TLASes still read it)
        FreeBuffer(&b.storage);
        b.as = as;
        b.storage = storage;
        b.address = address;
    }
    s.compacting.swap(still);
    if (!moved.empty()) {
        for (VkAccelerationStructureInstanceKHR& inst : s.instances) {
            auto mv = moved.find(inst.accelerationStructureReference);
            if (mv != moved.end()) {
                inst.accelerationStructureReference = mv->second;
            }
        }
        // (logged the first time, then every ten seconds or so)
        static size_t logged = 0;
        static uint64_t last_log_scene = 0;
        const bool first = logged == 0;
        logged += moved.size();
        if (first || s.scene - last_log_scene > 600) {
            last_log_scene = s.scene;
            LOGI << "Ray tracing: " << logged << " meshes' BLASes compacted so far" << std::endl;
        }
    }
    if (copied) {
        AsBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    }
}

// The scene's instances (those whose BLAS is built) in the next TLAS, built now; NULL: none
Tlas* BuildTlas(VkCommandBuffer cmd, Tlas& t) {
    std::vector<VkAccelerationStructureInstanceKHR> ready;
    ready.reserve(s.instances.size());
    for (size_t i = 0; i < s.instances.size(); ++i) {
        if (*s.instance_built[i]) {
            ready.push_back(s.instances[i]);
        }
    }
    const uint32_t count = (uint32_t)ready.size();
    VkAccelerationStructureGeometryKHR g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.arrayOfPointers = VK_FALSE;
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    if (t.as == VK_NULL_HANDLE || count > t.capacity) {
        FreeTlas(&t);
        const uint32_t capacity = std::max<uint32_t>(256, count + count / 2);
        VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(VkBackend::Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
        if (!MakeBuffer(capacity * sizeof(VkAccelerationStructureInstanceKHR), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true,
                        &t.instances) ||
            t.instances.mapped == NULL ||
            !MakeBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, &t.storage) ||
            !MakeBuffer(sizes.buildScratchSize + s.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &t.scratch)) {
            FreeTlas(&t);
            return NULL;
        }
        VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = t.storage.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(VkBackend::Device(), &ci, NULL, &t.as) != VK_SUCCESS) {
            t.as = VK_NULL_HANDLE;
            FreeTlas(&t);
            return NULL;
        }
        t.capacity = capacity;
    }
    if (count > 0) {
        memcpy(t.instances.mapped, ready.data(), count * sizeof(VkAccelerationStructureInstanceKHR));
        vmaFlushAllocation(VkBackend::Allocator(), t.instances.alloc, 0, VK_WHOLE_SIZE);
    }
    g.geometry.instances.data.deviceAddress = t.instances.address;
    info.dstAccelerationStructure = t.as;
    info.scratchData.deviceAddress = AlignUp(t.scratch.address, s.scratch_align);
    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR* range_ptr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &info, &range_ptr);
    t.serial = VkBackend::RecordingSerial();
    return &t;
}

// A colour texture's slot in set 1 (written the first time, or when its image was made
// again); kTextureSlots: none (not a plain 2D texture, or the slots are used up)
uint32_t TextureSlot(GLuint texture) {
    vkgl::ImageInfo info;
    if (texture == 0 || !vkgl::Image(texture, &info) || info.layers != 1 || info.samples != 1) {
        return kTextureSlots;
    }
    auto it = s.texture_slots.find(texture);
    if (it != s.texture_slots.end()) {
        const State::TextureSlotInfo& t = it->second;
        if (t.image == info.image && t.view == info.view && t.format == info.format && t.width == info.width && t.height == info.height &&
            t.levels == info.levels) {
            it->second.last_scene = s.scene;
            return t.slot;
        }
        // (its image made again: its old slot back, once what reads it now is done)
        s.free_slots.push_back({t.slot, VkBackend::RecordingSerial()});
        s.texture_slots.erase(it);
    }
    uint32_t slot = kTextureSlots;
    const uint64_t completed = VkBackend::CompletedSerial();
    for (size_t i = 0; i < s.free_slots.size(); ++i) {
        if (s.free_slots[i].serial <= completed) {
            slot = s.free_slots[i].slot;
            s.free_slots[i] = s.free_slots.back();
            s.free_slots.pop_back();
            break;
        }
    }
    if (slot == kTextureSlots) {
        if (s.next_slot >= kTextureSlots) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOGW << "Ray tracing: all " << kTextureSlots << " texture slots in use: further alpha-tested meshes are left out" << std::endl;
            }
            return kTextureSlots;
        }
        slot = s.next_slot++;
    }
    VkDescriptorImageInfo image = {s.texture_sampler, info.view, info.layout};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = s.textures_set;
    w.dstBinding = 0;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(VkBackend::Device(), 1, &w, 0, NULL);
    State::TextureSlotInfo t;
    t.image = info.image;
    t.view = info.view;
    t.format = info.format;
    t.width = info.width;
    t.height = info.height;
    t.levels = info.levels;
    t.slot = slot;
    t.last_scene = s.scene;
    s.texture_slots[texture] = t;
    return slot;
}

// A character's model copied for skinning, its BLAS made (built at its first skinning)
bool MakeSkinned(const Model& m, SkinnedBlas* b) {
    b->vertices = m.vertices.size() / 3;
    b->faces = m.faces.size() / 3;
    b->path = m.path;
    const VkDeviceSize position_bytes = b->vertices * 3 * sizeof(float);
    const VkDeviceSize vec4_bytes = b->vertices * 4 * sizeof(float);
    const VkDeviceSize index_bytes = b->faces * 3 * sizeof(uint32_t);
    const VkDeviceSize uv_bytes = b->vertices * 2 * sizeof(float);
    b->ids_offset = AlignUp(position_bytes, 16);
    b->weights_offset = b->ids_offset + vec4_bytes;
    b->index_offset = b->weights_offset + vec4_bytes;
    b->uv_offset = AlignUp(b->index_offset + index_bytes, 16);
    const VkDeviceSize input_bytes = b->uv_offset + uv_bytes;
    if (!MakeBuffer(input_bytes,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    false, &b->input) ||
        !MakeBuffer(input_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &b->staging) || b->staging.mapped == NULL ||
        !MakeBuffer(position_bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                    &b->positions)) {
        FreeSkinned(b);
        return false;
    }
    char* dst = (char*)b->staging.mapped;
    memcpy(dst, m.vertices.data(), position_bytes);
    memcpy(dst + b->ids_offset, m.bone_ids.data(), vec4_bytes);
    memcpy(dst + b->weights_offset, m.bone_weights.data(), vec4_bytes);
    memcpy(dst + b->index_offset, m.faces.data(), index_bytes);
    if (m.tex_coords.size() >= b->vertices * 2) {
        memcpy(dst + b->uv_offset, m.tex_coords.data(), uv_bytes);
    } else {
        memset(dst + b->uv_offset, 0, uv_bytes);
    }
    vmaFlushAllocation(VkBackend::Allocator(), b->staging.alloc, 0, VK_WHOLE_SIZE);

    VkAccelerationStructureGeometryKHR g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    g.geometry.triangles.vertexStride = 3 * sizeof(float);
    g.geometry.triangles.maxVertex = (uint32_t)b->vertices - 1;
    g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | DataAccess();
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    const uint32_t primitives = (uint32_t)b->faces;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(VkBackend::Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
    if (!MakeBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, &b->storage)) {
        FreeSkinned(b);
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = b->storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(VkBackend::Device(), &ci, NULL, &b->as) != VK_SUCCESS) {
        b->as = VK_NULL_HANDLE;
        FreeSkinned(b);
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = b->as;
    b->address = vkGetAccelerationStructureDeviceAddressKHR(VkBackend::Device(), &ai);
    b->build_scratch = sizes.buildScratchSize;
    b->update_scratch = sizes.updateScratchSize;
    b->built = false;
    b->refits = 0;
    return true;
}

// The scene's characters skinned and their BLASes built or refitted (before the TLAS)
void SkinAndRefit(VkCommandBuffer cmd, Tlas* t) {
    if (s.skins.empty() || s.skin == VK_NULL_HANDLE) {
        return;
    }
    // Their bones and morphs, in this TLAS set's upload (its last use is done)
    if (s.upload.size() > t->upload_capacity) {
        FreeBuffer(&t->upload);
        t->upload_capacity = 0;
        const VkDeviceSize capacity = std::max<VkDeviceSize>(1 << 20, s.upload.size() + s.upload.size() / 2);
        if (!MakeBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &t->upload) || t->upload.mapped == NULL) {
            FreeBuffer(&t->upload);
            return;
        }
        t->upload_capacity = capacity;
    }
    memcpy(t->upload.mapped, s.upload.data(), s.upload.size());
    vmaFlushAllocation(VkBackend::Allocator(), t->upload.alloc, 0, VK_WHOLE_SIZE);
    // Scratch for all of them at once
    VkDeviceSize scratch_total = 0;
    for (const Skin& k : s.skins) {
        const bool refit = k.blas->built && k.blas->refits < kRefitsBeforeRebuild;
        scratch_total += AlignUp(refit ? k.blas->update_scratch : k.blas->build_scratch, s.scratch_align);
    }
    if (scratch_total + s.scratch_align > t->skin_scratch_capacity) {
        FreeBuffer(&t->skin_scratch);
        t->skin_scratch_capacity = 0;
        const VkDeviceSize capacity = scratch_total + scratch_total / 2 + s.scratch_align;
        if (!MakeBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &t->skin_scratch)) {
            return;
        }
        t->skin_scratch_capacity = capacity;
    }
    // (new characters' bind pose and the rest into VRAM)
    bool uploaded = false;
    for (const Skin& k : s.skins) {
        if (k.blas->staging.buffer != VK_NULL_HANDLE) {
            VkBufferCopy copy = {0, 0, k.blas->input.size};
            vkCmdCopyBuffer(cmd, k.blas->staging.buffer, k.blas->input.buffer, 1, &copy);
            FreeBuffer(&k.blas->staging);  // (once these commands are done)
            uploaded = true;
        }
    }
    if (uploaded) {
        UploadBarrier(cmd);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.skin);
    for (const Skin& k : s.skins) {
        SkinnedBlas* b = k.blas;
        SkinPush push = {};
        push.positions = b->input.address;
        push.bone_ids = b->input.address + b->ids_offset;
        push.weights = b->input.address + b->weights_offset;
        push.bones = t->upload.address + k.bones_offset;
        push.has_morphs = k.morphs_offset != ~0ull ? 1u : 0u;
        push.morphs = push.has_morphs ? t->upload.address + k.morphs_offset : push.positions;
        push.out_positions = b->positions.address;
        push.vertices = (uint32_t)b->vertices;
        vkCmdPushConstants(cmd, s.skin_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (push.vertices + 63) / 64, 1, 1);
    }
    VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    m.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);

    const VkDeviceAddress base = AlignUp(t->skin_scratch.address, s.scratch_align);
    std::vector<VkAccelerationStructureGeometryKHR> geometries(s.skins.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(s.skins.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(s.skins.size());
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_ptrs(s.skins.size());
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < s.skins.size(); ++i) {
        SkinnedBlas* b = s.skins[i].blas;
        const bool refit = b->built && b->refits < kRefitsBeforeRebuild;
        VkAccelerationStructureGeometryKHR& g = geometries[i];
        g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = b->positions.address;
        g.geometry.triangles.vertexStride = 3 * sizeof(float);
        g.geometry.triangles.maxVertex = (uint32_t)b->vertices - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = b->input.address + b->index_offset;
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | DataAccess();
        info.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = refit ? b->as : VK_NULL_HANDLE;
        info.dstAccelerationStructure = b->as;
        info.geometryCount = 1;
        info.pGeometries = &g;
        info.scratchData.deviceAddress = base + offset;
        offset += AlignUp(refit ? b->update_scratch : b->build_scratch, s.scratch_align);
        ranges[i] = {};
        ranges[i].primitiveCount = (uint32_t)b->faces;
        range_ptrs[i] = &ranges[i];
        b->refits = refit ? b->refits + 1 : (b->built ? 0 : b->stagger);
        b->built = true;
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), range_ptrs.data());
}

// A plant's own BLAS made over its model's shared geometry (built at its first move)
bool MakePlant(const Model& m, const Blas& source, PlantBlas* b) {
    b->model = &m;
    b->source = &source;
    b->vertices = source.vertices;
    b->faces = source.faces;
    if (!MakeBuffer(b->vertices * 3 * sizeof(float), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    false, &b->positions)) {
        FreePlant(b);
        return false;
    }
    VkAccelerationStructureGeometryKHR g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    g.geometry.triangles.vertexStride = 3 * sizeof(float);
    g.geometry.triangles.maxVertex = (uint32_t)b->vertices - 1;
    g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | DataAccess();
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    const uint32_t primitives = (uint32_t)b->faces;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(VkBackend::Device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
    if (!MakeBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, &b->storage)) {
        FreePlant(b);
        return false;
    }
    VkAccelerationStructureCreateInfoKHR ci = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = b->storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(VkBackend::Device(), &ci, NULL, &b->as) != VK_SUCCESS) {
        b->as = VK_NULL_HANDLE;
        FreePlant(b);
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = b->as;
    b->address = vkGetAccelerationStructureDeviceAddressKHR(VkBackend::Device(), &ai);
    b->build_scratch = sizes.buildScratchSize;
    b->update_scratch = sizes.updateScratchSize;
    b->built = false;
    b->refits = 0;
    return true;
}

// The scene's near plants moved and their BLASes built or refitted (before the TLAS)
void DeformAndRefit(VkCommandBuffer cmd, Tlas* t) {
    // (a plant whose model's geometry isn't in VRAM yet, its shared BLAS not built: later;
    // its instance stays out of the TLAS meanwhile, its own BLAS not built either)
    s.plant_work.erase(std::remove_if(s.plant_work.begin(), s.plant_work.end(),
                                      [](const State::PlantWork& w) { return !w.source->built || w.source->staging.buffer != VK_NULL_HANDLE; }),
                       s.plant_work.end());
    if (s.plant_work.empty() || s.plant == VK_NULL_HANDLE) {
        return;
    }
    VkDeviceSize scratch_total = 0;
    for (const State::PlantWork& w : s.plant_work) {
        const bool refit = w.blas->built && w.blas->refits < kRefitsBeforeRebuild;
        scratch_total += AlignUp(refit ? w.blas->update_scratch : w.blas->build_scratch, s.scratch_align);
    }
    if (scratch_total + s.scratch_align > t->plant_scratch_capacity) {
        FreeBuffer(&t->plant_scratch);
        t->plant_scratch_capacity = 0;
        const VkDeviceSize capacity = scratch_total + scratch_total / 2 + s.scratch_align;
        if (!MakeBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &t->plant_scratch)) {
            return;
        }
        t->plant_scratch_capacity = capacity;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.plant);
    for (const State::PlantWork& w : s.plant_work) {
        vkCmdPushConstants(cmd, s.plant_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(w.push), &w.push);
        vkCmdDispatch(cmd, (w.push.vertices + 63) / 64, 1, 1);
    }
    VkMemoryBarrier2 m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    m.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    m.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    m.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);

    const VkDeviceAddress base = AlignUp(t->plant_scratch.address, s.scratch_align);
    const size_t n = s.plant_work.size();
    std::vector<VkAccelerationStructureGeometryKHR> geometries(n);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(n);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(n);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> range_ptrs(n);
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < n; ++i) {
        PlantBlas* b = s.plant_work[i].blas;
        const Blas* src = s.plant_work[i].source;
        const bool refit = b->built && b->refits < kRefitsBeforeRebuild;
        VkAccelerationStructureGeometryKHR& g = geometries[i];
        g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = b->positions.address;
        g.geometry.triangles.vertexStride = 3 * sizeof(float);
        g.geometry.triangles.maxVertex = (uint32_t)b->vertices - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress = src->geometry.address + src->index_offset;
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | DataAccess();
        info.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.srcAccelerationStructure = refit ? b->as : VK_NULL_HANDLE;
        info.dstAccelerationStructure = b->as;
        info.geometryCount = 1;
        info.pGeometries = &g;
        info.scratchData.deviceAddress = base + offset;
        offset += AlignUp(refit ? b->update_scratch : b->build_scratch, s.scratch_align);
        ranges[i] = {};
        ranges[i].primitiveCount = (uint32_t)b->faces;
        range_ptrs[i] = &ranges[i];
        b->refits = refit ? b->refits + 1 : (b->built ? 0 : b->stagger);
        b->built = true;
    }
    vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), range_ptrs.data());
}

// Meshes no longer drawn give their BLAS back (now and then)
void Forget() {
    if (s.scene % 120 != 0) {
        return;
    }
    for (auto it = s.blases.begin(); it != s.blases.end();) {
        if (it->second.last_scene + kForgetAfterScenes < s.scene) {
            s.pending.erase(std::remove(s.pending.begin(), s.pending.end(), it->first), s.pending.end());
            FreeBlas(&it->second);
            it = s.blases.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s.skinned.begin(); it != s.skinned.end();) {
        if (it->second.last_scene + kForgetAfterScenes < s.scene) {
            FreeSkinned(&it->second);
            it = s.skinned.erase(it);
        } else {
            ++it;
        }
    }
    // (texture slots no scene used for a while: a level changed, its textures gone)
    for (auto it = s.texture_slots.begin(); it != s.texture_slots.end();) {
        if (it->second.last_scene + kForgetAfterScenes < s.scene) {
            s.free_slots.push_back({it->second.slot, VkBackend::RecordingSerial()});
            it = s.texture_slots.erase(it);
        } else {
            ++it;
        }
    }
    // (a plant out of range for a while: back on its model's shared BLAS for good)
    for (auto it = s.plants.begin(); it != s.plants.end();) {
        if (it->second.last_scene + 120 < s.scene) {
            FreePlant(&it->second);
            it = s.plants.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

bool Available() {
    if (!s.tried && VkBackend::Ready()) {
        Start();
    }
    return s.available;
}

void BeginScene() {
    s.instances.clear();
    s.instance_built.clear();
    s.materials.clear();
    s.alpha_skipped = 0;
    s.skins.clear();
    s.upload.clear();
    s.plant_work.clear();
    ++s.scene;
}

// The model's shared BLAS, made the first time (built at the next trace); NULL: none
Blas* SharedBlas(const Model& model) {
    const size_t vertices = model.vertices.size() / 3, faces = model.faces.size() / 3;
    if (vertices == 0 || faces == 0) {
        return NULL;
    }
    Blas& b = s.blases[&model];
    if (b.as != VK_NULL_HANDLE && (b.vertices != vertices || b.faces != faces || b.path != model.path)) {
        // (another model where a freed one was, or one that changed)
        s.pending.erase(std::remove(s.pending.begin(), s.pending.end(), &model), s.pending.end());
        FreeBlas(&b);
    }
    if (b.as == VK_NULL_HANDLE) {
        if (!MakeBlas(model, &b)) {
            return NULL;
        }
        s.pending.push_back(&model);
    }
    b.last_scene = s.scene;
    return &b;
}

// A surface's texture slot (kNoTexture: none, or not there yet); false: an alpha-tested
// one whose texture isn't there yet (left out till it is)
bool SurfaceSlot(const Surface& surface, uint32_t* slot) {
    *slot = kNoTexture;
    if (surface.texture != 0) {
        const uint32_t got = TextureSlot(surface.texture);
        *slot = got == kTextureSlots ? kNoTexture : got;
    }
    if (surface.alpha_tested && *slot == kNoTexture) {
        ++s.alpha_skipped;
        return false;
    }
    return true;
}

// An instance's material entry (its instanceCustomIndex)
uint32_t AddMaterial(VkDeviceAddress indices, VkDeviceAddress uvs, uint32_t slot, const Surface& surface) {
    Material m = {};
    m.indices = indices;
    m.uvs = uvs;
    m.texture = slot;
    m.scroll = surface.scroll;
    m.flags = (surface.alpha_tested ? kMaterialAlphaTested : 0u) | (surface.emitter ? kMaterialEmitter : 0u) | (surface.waterfall ? kMaterialWaterfall : 0u) | (surface.grass ? kMaterialGrass : 0u);
    m.tint_mask = kNoTexture;
    if (surface.tint_mask != 0) {
        const uint32_t mask = TextureSlot(surface.tint_mask);
        m.tint_mask = mask == kTextureSlots ? kNoTexture : mask;
    }
    m.emission = surface.emitter ? surface.emission : 0.0f;
    m.tint[0] = surface.tint[0];
    m.tint[1] = surface.tint[1];
    m.tint[2] = surface.tint[2];
    s.materials.push_back(m);
    return (uint32_t)s.materials.size() - 1;
}

void AddMesh(const Model& model, const float t[16], const Surface& surface) {
    if (!s.available) {
        return;
    }
    const bool alpha_tested = surface.alpha_tested;
    const bool double_sided = surface.double_sided;
    const bool emitter = surface.emitter;
    uint32_t slot = kNoTexture;
    if (!SurfaceSlot(surface, &slot)) {
        return;  // (its texture isn't there yet)
    }
    Blas* shared = SharedBlas(model);
    if (shared == NULL) {
        return;
    }
    Blas& b = *shared;
    VkAccelerationStructureInstanceKHR inst = {};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            inst.transform.matrix[r][c] = t[c * 4 + r];
        }
    }
    inst.instanceCustomIndex = AddMaterial(b.geometry.address + b.index_offset, b.geometry.address + b.uv_offset, slot, surface);
    // (rt_shadow.comp's masks: opaque, alpha-tested, an opaque emitter the point lights' rays pass)
    inst.mask = alpha_tested ? 0x02 : emitter ? 0x04 : 0x01;
    inst.instanceShaderBindingTableRecordOffset = 0;
    // (a single-sided opaque mesh's faces can be culled: the point lights' rays pass the
    // faces turned away from the light; the rest stop every ray from either side)
    inst.flags = (alpha_tested || double_sided ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : kGlFacing) |
                 (alpha_tested ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    inst.accelerationStructureReference = b.address;
    s.instances.push_back(inst);
    s.instance_built.push_back(&b.built);
}

void AddMeshInstances(const Model& model, const float* transforms, size_t count, const Surface& surface) {
    if (!s.available || count == 0) {
        return;
    }
    uint32_t slot = kNoTexture;
    if (!SurfaceSlot(surface, &slot)) {
        return;
    }
    Blas* shared = SharedBlas(model);
    if (shared == NULL) {
        return;
    }
    Blas& b = *shared;
    const uint32_t material = AddMaterial(b.geometry.address + b.index_offset, b.geometry.address + b.uv_offset, slot, surface);
    const bool alpha_tested = surface.alpha_tested;
    VkAccelerationStructureInstanceKHR inst = {};
    inst.instanceCustomIndex = material;
    inst.mask = surface.grass ? 0x10 : alpha_tested ? 0x02 : surface.emitter ? 0x04 : 0x01;
    inst.flags = (alpha_tested || surface.double_sided ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : kGlFacing) |
                 (alpha_tested ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    inst.accelerationStructureReference = b.address;
    s.instances.reserve(s.instances.size() + count);
    s.instance_built.reserve(s.instance_built.size() + count);
    for (size_t i = 0; i < count; ++i) {
        const float* t = transforms + i * 16;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                inst.transform.matrix[r][c] = t[c * 4 + r];
            }
        }
        s.instances.push_back(inst);
        s.instance_built.push_back(&b.built);
    }
}

void AddPlant(const void* owner, const Model& model, const PlantPose& pose, const Surface& plant_surface, bool deform) {
    if (!s.available) {
        return;
    }
    Surface surface = plant_surface;
    surface.double_sided = true;  // (plants: either side)
    surface.scroll = pose.scroll;
    if (!deform || s.plant == VK_NULL_HANDLE) {
        AddMesh(model, pose.transform, surface);
        return;
    }
    const bool alpha_tested = surface.alpha_tested;
    uint32_t slot = kNoTexture;
    if (!SurfaceSlot(surface, &slot)) {
        return;
    }
    Blas* src = SharedBlas(model);
    if (src == NULL) {
        return;
    }
    PlantBlas& b = s.plants[owner];
    if (b.as != VK_NULL_HANDLE && (b.model != &model || b.source != src || b.vertices != src->vertices || b.faces != src->faces)) {
        FreePlant(&b);
    }
    if (b.as == VK_NULL_HANDLE) {
        if (!MakePlant(model, *src, &b)) {
            return;
        }
        b.stagger = (int)(((uintptr_t)owner >> 4) % kRefitsBeforeRebuild);
    }
    b.last_scene = s.scene;
    State::PlantWork w;
    w.blas = &b;
    w.source = src;
    w.push = {};
    for (int i = 0; i < 3; ++i) {
        w.push.scale[i] = pose.scale[i];
        w.push.translation[i] = pose.translation[i];
    }
    for (int i = 0; i < 4; ++i) {
        w.push.rotation[i] = pose.rotation[i];
    }
    w.push.scale[3] = pose.time;
    w.push.translation[3] = pose.shake;
    w.push.positions = src->geometry.address;
    w.push.aux = src->geometry.address + src->aux_offset;
    w.push.out_positions = b.positions.address;
    w.push.vertices = (uint32_t)b.vertices;
    w.push.waterfall = pose.waterfall ? 1u : 0u;
    s.plant_work.push_back(w);
    VkAccelerationStructureInstanceKHR inst = {};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;  // (moved to world space)
    inst.instanceCustomIndex = AddMaterial(src->geometry.address + src->index_offset, src->geometry.address + src->uv_offset, slot, surface);
    inst.mask = alpha_tested ? 0x02 : 0x01;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                 (alpha_tested ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR);
    inst.accelerationStructureReference = b.address;
    s.instances.push_back(inst);
    s.instance_built.push_back(&b.built);
}

void AddSkinnedMesh(const void* owner, const Model& model, const float* bones, size_t bone_count, GLuint texture) {
    if (!s.available || s.skin == VK_NULL_HANDLE || bones == NULL || bone_count == 0) {
        return;
    }
    const size_t vertices = model.vertices.size() / 3, faces = model.faces.size() / 3;
    if (vertices == 0 || faces == 0 || model.bone_ids.size() < vertices || model.bone_weights.size() < vertices) {
        return;
    }
    SkinnedBlas& b = s.skinned[std::make_pair(owner, &model)];
    if (b.as != VK_NULL_HANDLE && (b.vertices != vertices || b.faces != faces || b.path != model.path)) {
        FreeSkinned(&b);
    }
    if (b.as == VK_NULL_HANDLE) {
        if (!MakeSkinned(model, &b)) {
            return;
        }
        b.stagger = (int)(((uintptr_t)owner >> 4) % kRefitsBeforeRebuild);
    }
    b.last_scene = s.scene;
    Skin k;
    k.blas = &b;
    k.bones_offset = AlignUp(s.upload.size(), 16);
    s.upload.resize(k.bones_offset + bone_count * 16 * sizeof(float));
    memcpy(&s.upload[k.bones_offset], bones, bone_count * 16 * sizeof(float));
    k.morphs_offset = ~0ull;
    if (model.morph_transform_vec.size() >= vertices) {
        k.morphs_offset = AlignUp(s.upload.size(), 16);
        s.upload.resize(k.morphs_offset + vertices * 3 * sizeof(float));
        memcpy(&s.upload[k.morphs_offset], model.morph_transform_vec.data(), vertices * 3 * sizeof(float));
    }
    s.skins.push_back(k);
    VkAccelerationStructureInstanceKHR inst = {};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;  // (skinned to world space)
    Surface surface;
    surface.texture = texture;
    uint32_t slot = kNoTexture;
    SurfaceSlot(surface, &slot);
    inst.instanceCustomIndex = AddMaterial(b.input.address + b.index_offset, b.input.address + b.uv_offset, slot, surface);
    inst.mask = 0x01 | 0x08;  // (opaque, and a character: rt_shadow.comp's kCharacter finds one under a pixel)
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    inst.accelerationStructureReference = b.address;
    s.instances.push_back(inst);
    s.instance_built.push_back(&b.built);
}

int SceneInstances() {
    return (int)s.instances.size();
}

namespace {

// rt_nrd.comp's passes, made the first time (false: not there)
bool NrdPasses() {
    if (s.nrd_tried) {
        return s.nrd[0] != VK_NULL_HANDLE && s.nrd[1] != VK_NULL_HANDLE;
    }
    s.nrd_tried = true;
    const int kPathSize = 1024;
    char path[kPathSize];
    std::string source;
    if (FindFilePath("Data/GLSL/rt_nrd.comp", path, kPathSize, kDataPaths | kModPaths, false) == -1) {
        LOGE << "Ray tracing: no Data/GLSL/rt_nrd.comp: our own denoiser" << std::endl;
        return false;
    }
    textFileRead(path, &source);
    VkDevice dev = VkBackend::Device();
    // PREPARE writes the shared geometry guides and noisy shadow/AO inputs.
    // COMPOSITE packs the denoised signals into the colour pass's lighting mask.
    const uint32_t images[2] = {9, 5};
    const uint32_t push_sizes[2] = {sizeof(NrdPreparePush), 4 * sizeof(uint32_t)};
    const char* defines[2] = {"#define PREPARE 1\n", "#define COMPOSITE 1\n"};
    const char* names[2] = {"rt_nrd.comp #PREPARE", "rt_nrd.comp #COMPOSITE"};
    for (int i = 0; i < 2; ++i) {
        VkDescriptorSetLayoutBinding b[16] = {};
        for (uint32_t k = 0; k < images[i]; ++k) {
            b[k].binding = k;
            b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[k].descriptorCount = 1;
            b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = images[i];
        li.pBindings = b;
        VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_sizes[i]};
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s.nrd_set_layout[i];
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        if (vkCreateDescriptorSetLayout(dev, &li, NULL, &s.nrd_set_layout[i]) != VK_SUCCESS ||
            vkCreatePipelineLayout(dev, &pli, NULL, &s.nrd_layout[i]) != VK_SUCCESS) {
            return false;
        }
        s.nrd[i] = MakeComputePipeline(source, defines[i], names[i], s.nrd_layout[i]);
        if (s.nrd[i] == VK_NULL_HANDLE) {
            return false;
        }
    }
    return true;
}

// The pass's storage images pushed (binding i: views[i]) and dispatched over the scene
void DispatchImages(VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout, const VkImageView* views, uint32_t count, const void* push,
                    uint32_t push_size, uint32_t width, uint32_t height) {
    VkDescriptorImageInfo infos[16];
    VkWriteDescriptorSet w[16] = {};
    for (uint32_t i = 0; i < count; ++i) {
        infos[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[i].pImageInfo = &infos[i];
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, count, w);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}

}  // namespace

bool NrdAvailable() {
    return Available() && VkNrd::Available();
}

uint32_t LastNrdDispatchCount() {
    return s.nrd_dispatches;
}

bool GiSupported() {
    return DrawsCanTrace() && VkBackend::Properties().limits.maxPushConstantsSize >= sizeof(GiPush);
}

bool GiTraced() {
    return s.available && s.gi_drawn;
}

bool DrawsCanTrace() {
    return Available() && VkBackend::RtPositionFetch();
}

bool SceneForDraws(DrawScene* out) {
    if (!s.available || !s.draw_scene || s.drawn.tlas == VK_NULL_HANDLE || s.drawn_set == NULL) {
        return false;
    }
    // (the set waits for these commands before it's written again)
    s.drawn_set->serial = std::max(s.drawn_set->serial, VkBackend::RecordingSerial());
    *out = s.drawn;
    return true;
}

VkDescriptorSetLayout TexturesLayout() {
    return Available() ? s.textures_layout : VK_NULL_HANDLE;
}

void EndScene() {
    s.draw_scene = false;
}

namespace {

// rt_gi.comp's and rt_cache.comp's passes and the radiance cache, made the first time
// (false: not there)
bool GiPasses() {
    if (s.gi_tried) {
        return s.cache_ages.buffer != VK_NULL_HANDLE;
    }
    s.gi_tried = true;
    if (!GiSupported()) {
        LOGI << "Ray tracing: GI needs the hits' triangles (position fetch) and bigger push constants" << std::endl;
        return false;
    }
    const int kPathSize = 1024;
    char path[kPathSize];
    std::string gi_source, cache_source;
    if (FindFilePath("Data/GLSL/rt_gi.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &gi_source);
    }
    if (FindFilePath("Data/GLSL/rt_cache.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &cache_source);
    }
    if (gi_source.empty() || cache_source.empty()) {
        LOGE << "Ray tracing: no Data/GLSL/rt_gi.comp or rt_cache.comp: no GI" << std::endl;
        return false;
    }
    VkDevice dev = VkBackend::Device();
    // rt_gi.comp: the scene, the depth, the materials, the lights, the sky, its raw result,
    // the cache's keys, sums and averages, the rays' directions (set 0, pushed); the
    // texture slots (set 1)
    // (and the light probes' grid)
    const VkDescriptorType gi_types[11] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding gb[11] = {};
    for (uint32_t i = 0; i < 11; ++i) {
        gb[i].binding = i;
        gb[i].descriptorType = gi_types[i];
        gb[i].descriptorCount = 1;
        gb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo gli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    gli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    gli.bindingCount = 11;
    gli.pBindings = gb;
    VkPushConstantRange gi_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GiPush)};
    // rt_cache.comp: the cache's keys, sums, averages, ages
    VkDescriptorSetLayoutBinding cb[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        cb[i].binding = i;
        cb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cb[i].descriptorCount = 1;
        cb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo cli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    cli.bindingCount = 4;
    cli.pBindings = cb;
    VkPushConstantRange cache_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(uint32_t)};
    if (vkCreateDescriptorSetLayout(dev, &gli, NULL, &s.gi_set_layout) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(dev, &cli, NULL, &s.cache_set_layout) != VK_SUCCESS) {
        return false;
    }
    const VkDescriptorSetLayout gi_sets[2] = {s.gi_set_layout, s.textures_layout};
    VkPipelineLayoutCreateInfo gpli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    gpli.setLayoutCount = 2;
    gpli.pSetLayouts = gi_sets;
    gpli.pushConstantRangeCount = 1;
    gpli.pPushConstantRanges = &gi_range;
    VkPipelineLayoutCreateInfo cpli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cpli.setLayoutCount = 1;
    cpli.pSetLayouts = &s.cache_set_layout;
    cpli.pushConstantRangeCount = 1;
    cpli.pPushConstantRanges = &cache_range;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreatePipelineLayout(dev, &gpli, NULL, &s.gi_layout) != VK_SUCCESS || vkCreatePipelineLayout(dev, &cpli, NULL, &s.cache_layout) != VK_SUCCESS ||
        vkCreateSampler(dev, &si, NULL, &s.sky_sampler) != VK_SUCCESS) {
        return false;
    }
    s.gi[0] = MakeComputePipeline(gi_source, "", "rt_gi.comp", s.gi_layout);
    s.gi[1] = MakeComputePipeline(gi_source, "#define MSAA_DEPTH 1\n", "rt_gi.comp #MSAA_DEPTH", s.gi_layout);
    s.cache = MakeComputePipeline(cache_source, "", "rt_cache.comp", s.cache_layout);
    std::string filter_source;
    if (FindFilePath("Data/GLSL/rt_gi_denoise.comp", path, kPathSize, kDataPaths | kModPaths, false) != -1) {
        textFileRead(path, &filter_source);
        s.gi_denoise = MakeComputePipeline(filter_source, "", "rt_gi_denoise.comp", s.denoise_layout);
    }
    if (s.gi[0] == VK_NULL_HANDLE || s.gi[1] == VK_NULL_HANDLE || s.cache == VK_NULL_HANDLE) {
        return false;
    }
    // (the cache: 4 + 16 + 16 + 4 bytes a voxel)
    if (!MakeBuffer(kCacheVoxels * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &s.cache_keys) ||
        !MakeBuffer(kCacheVoxels * 4 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &s.cache_accum) ||
        !MakeBuffer(kCacheVoxels * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &s.cache_values) ||
        !MakeBuffer(kCacheVoxels * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, &s.cache_ages)) {
        LOGE << "Ray tracing: couldn't make GI's radiance cache" << std::endl;
        return false;
    }
    s.cache_fresh = true;
    LOGI << "Ray tracing: GI on (a radiance cache of " << kCacheVoxels << " voxels)" << std::endl;
    return true;
}

void ComputeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// The cache's pass over its voxels (clear: empty them all)
void DispatchCache(VkCommandBuffer cmd, bool clear) {
    VkDescriptorBufferInfo infos[4] = {{s.cache_keys.buffer, 0, VK_WHOLE_SIZE},
                                       {s.cache_accum.buffer, 0, VK_WHOLE_SIZE},
                                       {s.cache_values.buffer, 0, VK_WHOLE_SIZE},
                                       {s.cache_ages.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    // (a voxel's average over 32 frames at most; one nothing fed for 240 frames goes)
    const uint32_t info[4] = {32u, 240u, clear ? 1u : 0u, 0u};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.cache);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.cache_layout, 0, 4, w);
    vkCmdPushConstants(cmd, s.cache_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(info), info);
    vkCmdDispatch(cmd, kCacheVoxels / 64, 1, 1);
}

// GI traced into its raw result (and the cache fed and averaged): false: not traced
bool TraceGi(VkCommandBuffer cmd, const LightingPass& p, Tlas* t, const vkgl::ImageInfo& depth, const vkgl::ImageInfo& gi_raw,
             const vkgl::ImageInfo& gi_dir, uint32_t light_count) {
    vkgl::ImageInfo sky;
    if (!p.gi || p.sky == 0 || !GiPasses() || !vkgl::Image(p.sky, &sky) || sky.layers != 6) {
        return false;
    }
    // The light probes' grid into VRAM when it's new (none: a header saying so; the cache
    // emptied, as what it holds was lit by the last one)
    bool probes_new = false;
    if (s.probes.buffer == VK_NULL_HANDLE || p.probe_grid_version != s.probes_version) {
        static const uint32_t kNoGrid[16] = {};
        const bool given = p.probe_grid != nullptr && p.probe_grid_words >= 16;
        const uint32_t* data = given ? p.probe_grid : kNoGrid;
        const VkDeviceSize bytes = (given ? p.probe_grid_words : 16) * sizeof(uint32_t);
        FreeBuffer(&s.probes_staging);
        if (s.probes.size < bytes) {
            FreeBuffer(&s.probes);
        }
        if ((s.probes.buffer != VK_NULL_HANDLE ||
             MakeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, &s.probes)) &&
            MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, &s.probes_staging) && s.probes_staging.mapped) {
            memcpy(s.probes_staging.mapped, data, (size_t)bytes);
            vmaFlushAllocation(VkBackend::Allocator(), s.probes_staging.alloc, 0, VK_WHOLE_SIZE);
            VkBufferCopy region = {0, 0, bytes};
            vkCmdCopyBuffer(cmd, s.probes_staging.buffer, s.probes.buffer, 1, &region);
            UploadBarrier(cmd);
            s.probes_version = p.probe_grid_version;
            probes_new = true;
        } else {
            return false;
        }
    }
    if (s.cache_fresh || p.gi_cache_reset || probes_new) {
        DispatchCache(cmd, true);
        ComputeBarrier(cmd);
        s.cache_fresh = false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.gi[depth.samples > 1 ? 1 : 0]);
    VkWriteDescriptorSetAccelerationStructureKHR as_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &t->as;
    VkDescriptorImageInfo depth_info = {s.sampler, depth.view, depth.layout};
    VkDescriptorBufferInfo materials_info = {t->materials.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lights_info = {t->lights.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo sky_info = {s.sky_sampler, sky.view, sky.layout};
    VkDescriptorImageInfo out_info = {VK_NULL_HANDLE, gi_raw.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo keys_info = {s.cache_keys.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo accum_info = {s.cache_accum.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo values_info = {s.cache_values.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo dir_info = {VK_NULL_HANDLE, gi_dir.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo probes_info = {s.probes.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[11] = {};
    for (int i = 0; i < 11; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
    }
    w[0].pNext = &as_write;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &depth_info;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].pBufferInfo = &materials_info;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[3].pBufferInfo = &lights_info;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[4].pImageInfo = &sky_info;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[5].pImageInfo = &out_info;
    w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[6].pBufferInfo = &keys_info;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[7].pBufferInfo = &accum_info;
    w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[8].pBufferInfo = &values_info;
    w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[9].pImageInfo = &dir_info;
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[10].pBufferInfo = &probes_info;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.gi_layout, 0, 11, w);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.gi_layout, 1, 1, &s.textures_set, 0, NULL);
    GiPush push = {};
    memcpy(push.inv_view_proj, p.inv_view_proj, sizeof(push.inv_view_proj));
    push.camera[0] = p.camera[0];
    push.camera[1] = p.camera[1];
    push.camera[2] = p.camera[2];
    push.camera[3] = p.bias;
    push.sun[0] = p.sun[0];
    push.sun[1] = p.sun[1];
    push.sun[2] = p.sun[2];
    push.sun[3] = std::max(0.0f, std::min(1.0f, p.gi_lights));
    push.sun_color[0] = p.sun_color[0];
    push.sun_color[1] = p.sun_color[1];
    push.sun_color[2] = p.sun_color[2];
    push.sun_color[3] = p.time;
    push.info[0] = p.width;
    push.info[1] = p.height;
    push.info[2] = p.frame;
    push.info[3] = light_count;
    push.params[0] = std::max(0.05f, p.gi_voxel);
    push.params[1] = p.gi_emission;
    push.params[2] = p.gi_sky_lod;
    push.params[3] = p.gi_sky_scale;
    push.debug[0] = p.gi_debug;
    push.debug[1] = VkNrd::FrameIndex() & 1u;
    const bool checkerboard = p.nrd && !p.gi_rr;
    push.debug[2] = checkerboard ? 1u : 0u;  // (NRD's REBLUR reconstructs the other half: checkerboardMode BLACK)
    push.look[0] = std::max(0.0f, p.gi_strength);
    push.look[1] = std::max(0.0f, p.ambient_contrib);
    push.look[2] = std::max(0.0f, p.gi_bounce);
    push.look[3] = std::max(0.0f, std::min(1.0f, p.gi_raster_ambient));
    vkCmdPushConstants(cmd, s.gi_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, ((checkerboard ? (p.width + 1) / 2 : p.width) + 7) / 8, (p.height + 7) / 8, 1);  // (NRD's: half the pixels, a checkerboard)
    // (the cache's sums into its averages, after the samples are in)
    ComputeBarrier(cmd);
    DispatchCache(cmd, false);
    return true;
}

// NRD's way: rt_nrd.comp #PREPARE, NRD's passes, #COMPOSITE into the mask (false: not
// there, nothing recorded: our own denoiser then)
bool DenoiseNrd(VkCommandBuffer cmd, const LightingPass& p, const vkgl::ImageInfo* images, int ring, const vkgl::ImageInfo* gi_images) {
    if (!p.nrd || !p.denoise || !VkNrd::Available() || !NrdPasses() || !VkNrd::Prepare(cmd, p.width, p.height)) {
        return false;
    }
    const uint32_t signals = (p.sun_rays > 0 ? 1u : 0u) | (p.ao_rays > 0 ? 2u : 0u) | (p.light_rays > 0 && p.light_count > 0 ? 4u : 0u) |
                             (gi_images != NULL ? 8u : 0u);
    VkMemoryBarrier2 traced = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    traced.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    traced.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    traced.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    traced.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo traced_dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    traced_dep.memoryBarrierCount = 1;
    traced_dep.pMemoryBarriers = &traced;
    vkCmdPipelineBarrier2(cmd, &traced_dep);

    NrdPreparePush pp = {};
    memcpy(pp.inv_view_proj, p.inv_view_proj, sizeof(pp.inv_view_proj));
    memcpy(pp.cur, p.view_proj, sizeof(pp.cur));
    memcpy(pp.prev, p.prev_view_proj, sizeof(pp.prev));
    VkNrd::Frame f;
    f.denoising_range = 100000.0f;
    pp.params[0] = tanf(p.cone);
    pp.params[1] = -2.0f * f.denoising_range;  // (the sky: beyond it)
    pp.info[0] = p.width;
    pp.info[1] = p.height;
    pp.info[2] = signals;
    const VkImageView prepare_views[9] = {images[0].view,
                                           images[5].view,
                                           VkNrd::View(VkNrd::kViewZ),
                                           VkNrd::View(VkNrd::kNormalRoughness),
                                           VkNrd::View(VkNrd::kMotion),
                                           VkNrd::View(VkNrd::kSunPenumbra),
                                           VkNrd::View(VkNrd::kLightPenumbra),
                                           VkNrd::View(VkNrd::kLightTranslucency),
                                           VkNrd::View(VkNrd::kAoHitDist)};
    DispatchImages(cmd, s.nrd[0], s.nrd_layout[0], prepare_views, 9, &pp, sizeof(pp), p.width, p.height);

    f.width = p.width;
    f.height = p.height;
    memcpy(f.view, p.nrd_view, sizeof(f.view));
    memcpy(f.proj, p.nrd_proj, sizeof(f.proj));
    memcpy(f.view_prev, p.nrd_view_prev, sizeof(f.view_prev));
    memcpy(f.proj_prev, p.nrd_proj_prev, sizeof(f.proj_prev));
    f.sun[0] = p.sun[0];
    f.sun[1] = p.sun[1];
    f.sun[2] = p.sun[2];
    f.ao_range = p.ao_range;
    f.sun_shadow = (signals & 1u) != 0;
    f.ao = (signals & 2u) != 0;
    f.lights = (signals & 4u) != 0;
    f.gi = gi_images != NULL;
    if (f.gi) {
        f.gi_sh0_in = gi_images[0].view;
        f.gi_sh1_in = gi_images[1].view;
        f.gi_sh0_out = gi_images[2].view;
        f.gi_sh1_out = gi_images[3].view;
    }
    f.reset = p.nrd_reset;
    // (NRD's passes end with a barrier: its outputs ready to read)
    s.nrd_dispatches = signals != 0 ? VkNrd::Denoise(cmd, f, ring) : 0;

    const uint32_t composite_info[4] = {p.width, p.height, s.nrd_dispatches ? signals : 0u, 0u};
    const VkImageView composite_views[5] = {images[0].view,
                                            VkNrd::View(VkNrd::kSunShadow),
                                            VkNrd::View(VkNrd::kLightShadow),
                                            VkNrd::View(VkNrd::kAoOut),
                                            images[4].view};
    DispatchImages(cmd, s.nrd[1], s.nrd_layout[1], composite_views, 5, composite_info, sizeof(composite_info), p.width, p.height);
    return true;
}

}  // namespace

bool TraceLighting(const LightingPass& p) {
    s.nrd_dispatches = 0;
    s.gi_drawn = false;
    if (!Available() || p.depth == 0 || p.raw == 0 || p.raw_prev == 0 || p.history_prev == 0 || p.history == 0 || p.mask == 0 || p.extra == 0 ||
        p.width == 0 || p.height == 0) {
        return false;
    }
    s.draw_scene = false;  // (handed to draws again only if this scene's trace gets that far)
    Forget();
    vkgl::ImageInfo depth;
    if (!vkgl::BeginExternal(p.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, &depth)) {
        return false;
    }
    // (raw, last frame's raw, last frame's history, this frame's history, the mask, the penumbras' sizes)
    const int kImages = 6;
    const GLuint names[kImages] = {p.raw, p.raw_prev, p.history_prev, p.history, p.mask, p.extra};
    vkgl::ImageInfo images[kImages];
    int begun = 0;
    for (; begun < kImages; ++begun) {
        if (!vkgl::BeginExternal(names[begun], VK_IMAGE_LAYOUT_GENERAL, &images[begun])) {
            break;
        }
    }
    if (begun < kImages) {
        for (int i = 0; i < begun; ++i) {
            vkgl::EndExternal(names[i]);
        }
        vkgl::EndExternal(p.depth);
        return false;
    }
    // (GI's raw result and directions, what the colour pass reads of it, where it's asked for)
    const bool builtin_gi = !p.nrd && !p.gi_rr;
    const int gi_image_count = builtin_gi ? 6 : 4;
    vkgl::ImageInfo gi_images[6];
    const GLuint gi_names[] = {p.gi_raw, p.gi_dir, p.gi_sh0, p.gi_sh1, p.gi_sh0_prev, p.gi_sh1_prev};
    bool gi_begun = false;
    if (p.gi && p.gi_raw != 0 && p.gi_dir != 0 && p.gi_sh0 != 0 && p.gi_sh1 != 0) {
        int gi_count = 0;
        while (gi_count < gi_image_count && vkgl::BeginExternal(gi_names[gi_count], VK_IMAGE_LAYOUT_GENERAL, &gi_images[gi_count])) {
            ++gi_count;
        }
        gi_begun = gi_count == gi_image_count;
        if (!gi_begun) {
            for (int i = 0; i < gi_count; ++i) {
                vkgl::EndExternal(gi_names[i]);
            }
        }
    }
    bool gi_traced = false;
    VkCommandBuffer cmd = VkBackend::TransferCommands();
    // (the last scene's traces and refits done before this one's skinning and refits
    // write what they read)
    VkMemoryBarrier2 before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    // (and the colour pass's reflections, which read them too)
    before.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    before.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    before.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    VkDependencyInfo before_dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    before_dep.memoryBarrierCount = 1;
    before_dep.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(cmd, &before_dep);
    Tlas& set = s.tlas[s.next_tlas];
    const int ring = s.next_tlas;  // (NRD's constants: the part this set's last use is done with)
    s.next_tlas = (s.next_tlas + 1) % kTlasSets;
    if (set.serial > VkBackend::CompletedSerial()) {
        VkBackend::WaitSerial(set.serial);
    }
    CompactBuilt(cmd);
    BuildPending(cmd);
    SkinAndRefit(cmd, &set);
    DeformAndRefit(cmd, &set);
    AsBarrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
    Tlas* t = BuildTlas(cmd, set);
    bool ok = t != NULL;
    {
        // (logged when the scene changes: a level loaded, meshes coming and going)
        // (not when only characters and plants come and go: they do every second)
        static size_t logged_instances = 0, logged_blases = 0, logged_skinned = 0;
        if (s.instances.size() != logged_instances || s.blases.size() != logged_blases) {
            logged_instances = s.instances.size();
            logged_blases = s.blases.size();
            logged_skinned = s.skinned.size();
            size_t alpha_tested = 0;
            for (const Material& m : s.materials) {
                alpha_tested += (m.flags & kMaterialAlphaTested) != 0 ? 1 : 0;
            }
            LOGI << "Ray tracing: " << s.instances.size() << " instances (" << alpha_tested << " alpha-tested, " << s.alpha_skipped
                 << " waiting for their texture), " << s.blases.size() << " meshes, " << s.skinned.size() << " skinned, " << s.plants.size()
                 << " plants moving" << std::endl;
        }
    }
    if (ok) {
        // The scene's materials (at least one entry: the binding is always there)
        const uint32_t material_count = (uint32_t)std::max<size_t>(s.materials.size(), 1);
        if (material_count > t->materials_capacity) {
            FreeBuffer(&t->materials);
            t->materials_capacity = 0;
            const uint32_t capacity = std::max<uint32_t>(64, material_count + material_count / 2);
            if (MakeBuffer(capacity * sizeof(Material), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &t->materials) && t->materials.mapped) {
                t->materials_capacity = capacity;
            }
        }
        ok = t->materials_capacity > 0;
    }
    const uint32_t light_count = p.lights != nullptr && (p.light_rays > 0 || p.gi) ? p.light_count : 0;
    if (ok) {
        // The point lights (at least one entry too)
        const uint32_t count = std::max<uint32_t>(light_count, 1);
        if (count > t->lights_capacity) {
            FreeBuffer(&t->lights);
            t->lights_capacity = 0;
            const uint32_t capacity = std::max<uint32_t>(64, count + count / 2);
            if (MakeBuffer(capacity * 8 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, &t->lights) && t->lights.mapped) {
                t->lights_capacity = capacity;
            }
        }
        ok = t->lights_capacity > 0;
    }
    if (ok) {
        if (!s.materials.empty()) {
            memcpy(t->materials.mapped, s.materials.data(), s.materials.size() * sizeof(Material));
            vmaFlushAllocation(VkBackend::Allocator(), t->materials.alloc, 0, VK_WHOLE_SIZE);
        }
        if (light_count > 0) {
            memcpy(t->lights.mapped, p.lights, light_count * 8 * sizeof(float));
            vmaFlushAllocation(VkBackend::Allocator(), t->lights.alloc, 0, VK_WHOLE_SIZE);
        }
        // (the TLAS read by our passes and by the colour pass's reflections after them)
        AsBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        s.draw_scene = true;
        s.drawn_set = t;
        s.drawn.tlas = t->as;
        s.drawn.materials = t->materials.buffer;
        s.drawn.textures = s.textures_set;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.shadow[depth.samples > 1 ? 1 : 0]);
        VkWriteDescriptorSetAccelerationStructureKHR as_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &t->as;
        VkDescriptorImageInfo depth_info = {s.sampler, depth.view, depth.layout};
        VkDescriptorImageInfo mask_info = {VK_NULL_HANDLE, images[0].view, VK_IMAGE_LAYOUT_GENERAL};  // (the raw result)
        VkDescriptorBufferInfo alpha_info = {t->materials.buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo extra_info = {VK_NULL_HANDLE, images[5].view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo lights_info = {t->lights.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[6] = {};
        for (int i = 0; i < 6; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = (uint32_t)i;
            w[i].descriptorCount = 1;
        }
        w[0].pNext = &as_write;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &depth_info;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[2].pImageInfo = &mask_info;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &alpha_info;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &extra_info;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[5].pBufferInfo = &lights_info;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 0, 6, w);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.layout, 1, 1, &s.textures_set, 0, NULL);
        ShadowPush push = {};
        memcpy(push.inv_view_proj, p.inv_view_proj, sizeof(push.inv_view_proj));
        push.sun[0] = p.sun[0];
        push.sun[1] = p.sun[1];
        push.sun[2] = p.sun[2];
        push.sun[3] = p.cone;
        push.info[0] = p.width;
        push.info[1] = p.height;
        push.info[2] = p.frame;
        const uint32_t light_rays = (light_count > 0 ? (uint32_t)std::max(0, std::min(0x7F, p.light_rays)) : 0) | (p.light_cull ? 0u : 0x80u);
        const uint32_t clearance = (uint32_t)std::max(0, std::min(0xFF, (int)(p.light_clearance * 100.0f + 0.5f)));
        push.info[3] = (uint32_t)std::max(0, std::min(0xFF, p.sun_rays)) | ((uint32_t)std::max(0, std::min(0xFF, p.ao_rays)) << 8) |
                       (light_rays << 16) | (clearance << 24);
        push.camera[0] = p.camera[0];
        push.camera[1] = p.camera[1];
        push.camera[2] = p.camera[2];
        push.camera[3] = p.bias;
        push.ao[0] = p.ao_range;
        push.ao[1] = p.time;
        push.ao[2] = (float)light_count;
        push.ao[3] = std::max(0.0f, p.light_size);
        vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        {
            PROFILER_GPU_ZONE(g_profiler_ctx, "RT shadows");
            vkCmdDispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
        }
        // Trace checkerboard samples for NRD, or full-resolution spherical harmonics otherwise.
        PROFILER_GPU_ZONE(g_profiler_ctx, "RT GI");
        gi_traced = gi_begun && TraceGi(cmd, p, t, depth, gi_images[0], gi_images[1], light_count);
    }
    // NRD's denoisers where they're there (and asked for), else ours
    PROFILER_GPU_ZONE(g_profiler_ctx, "RT denoise");
    if (ok && DenoiseNrd(cmd, p, images, ring, gi_traced && !p.gi_rr ? gi_images : NULL)) {
        s.gi_drawn = gi_traced && (p.gi_rr || s.nrd_dispatches > 0);
    } else if (ok) {
        s.gi_drawn = gi_traced && p.gi_rr;
        if (gi_traced && builtin_gi && s.gi_denoise != VK_NULL_HANDLE) {
            ComputeBarrier(cmd);
            DenoisePush filter = {};
            memcpy(filter.reprojection, p.reprojection, sizeof(filter.reprojection));
            filter.info[0] = p.width;
            filter.info[1] = p.height;
            filter.info[3] = (p.gi_history ? 1u : 0u) | (p.denoise ? 2u : 0u);
            // The radiance and its direction use identical depth weights.
            for (int i = 0; i < 2; ++i) {
                const VkImageView views[] = {images[0].view, images[1].view, gi_images[i].view,
                                             gi_images[4 + i].view, gi_images[2 + i].view};
                DispatchImages(cmd, s.gi_denoise, s.denoise_layout, views, 5, &filter, sizeof(filter), p.width, p.height);
            }
            s.gi_drawn = true;
        }
        // The denoiser: over time into the history, then across the penumbra into the mask
        DenoisePush dp = {};
        memcpy(dp.reprojection, p.reprojection, sizeof(dp.reprojection));
        dp.info[0] = p.width;
        dp.info[1] = p.height;
        dp.info[2] = p.frame;
        dp.info[3] = (p.has_history ? 1u : 0u) | (p.denoise ? 2u : 0u);
        dp.params[0] = 0.1f;   // (a new frame counts a tenth at least: shadows that move follow)
        dp.params[1] = 32.0f;  // (the most frames the history holds)
        dp.params[2] = p.near_plane;
        dp.params[3] = p.pixel_angle;
        dp.extra[0] = 2.0f * tanf(p.cone);
        dp.extra[1] = p.ao_range;
        VkMemoryBarrier2 between = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        between.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        between.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        between.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        between.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo between_dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        between_dep.memoryBarrierCount = 1;
        between_dep.pMemoryBarriers = &between;
        // (pass 0 reads last frame's history and writes this frame's; pass 1 reads this frame's)
        const int order[2][kImages] = {{0, 1, 2, 3, 4, 5}, {0, 1, 3, 2, 4, 5}};
        for (int pass = 0; pass < 2; ++pass) {
            vkCmdPipelineBarrier2(cmd, &between_dep);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.denoise[pass]);
            VkDescriptorImageInfo infos[kImages];
            VkWriteDescriptorSet dw[kImages] = {};
            for (int i = 0; i < kImages; ++i) {
                infos[i] = {VK_NULL_HANDLE, images[order[pass][i]].view, VK_IMAGE_LAYOUT_GENERAL};
                dw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                dw[i].dstBinding = (uint32_t)i;
                dw[i].descriptorCount = 1;
                dw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                dw[i].pImageInfo = &infos[i];
            }
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.denoise_layout, 0, kImages, dw);
            vkCmdPushConstants(cmd, s.denoise_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dp), &dp);
            vkCmdDispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
        }
    }
    for (int i = 0; i < kImages; ++i) {
        vkgl::EndExternal(names[i]);
    }
    if (gi_begun) {
        for (int i = 0; i < gi_image_count; ++i) {
            vkgl::EndExternal(gi_names[i]);
        }
    }
    vkgl::EndExternal(p.depth);
    return ok;
}

}  // namespace VkRt
