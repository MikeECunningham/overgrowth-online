//-----------------------------------------------------------------------------
//           Name: raytrace.cpp
//      Developer: Overgrowth Online (crxhvrd)
//    Description: See raytrace.h
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#include "raytrace.h"

#include <Graphics/camera.h>
#include <Graphics/detailobjectsurface.h>
#include <Graphics/graphics.h>
#include <Graphics/models.h>
#include <Graphics/textures.h>
#include <Graphics/render_backend.h>
#include <Graphics/sky.h>
#include <Graphics/temporal.h>
#include <Graphics/Vulkan/vk_backend.h>
#include <Graphics/Vulkan/vk_gl.h>
#include <Graphics/Vulkan/vk_rt.h>
#include <Editors/entity_type.h>
#include <Internal/config.h>
#include <Internal/profiler.h>
#include <Internal/timer.h>
#include <Logging/logdata.h>
#include <Main/scenegraph.h>
#include <Math/quaternions.h>
#include <Objects/dynamiclightobject.h>
#include <Objects/decalobject.h>
#include <Objects/envobject.h>
#undef near  // (windows.h's, through decalobject.h: a name below)
#undef far
#include <Objects/itemobject.h>
#include <Objects/movementobject.h>
#include <Objects/riggedobject.h>
#include <Objects/terrainobject.h>

#include <algorithm>
#include <cfloat>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <string>

extern Config config;
extern Timer game_timer;

namespace Raytrace {
namespace {

struct State {
    // At the scene's size (VkRt::LightingPass): the raw result and the denoiser's history,
    // this frame's [current] and last frame's; the mask the colour pass reads; what sizes
    // the penumbras
    TextureRef raw[2], history[2], mask, extra;
    // GI's (rt_gi): its raw result and directions (NRD's input), what the colour pass reads
    // (REBLUR's SH0 and SH1)
    TextureRef gi_raw, gi_dir, gi_sh0, gi_sh1;
    bool gi_ready = false;
    bool gi_rr = false;  // (this scene's GI is raw, for DLSS Ray Reconstruction: gi_raw and gi_dir)
    std::string gi_level;  // (the level the radiance cache holds: another empties it)
    std::vector<float> lights;  // (the scene's point lights in view, LightingPass::lights)
    std::vector<float> grass;   // (a grass surface's instances' transforms, this frame's)
    size_t grass_count = 0;
    // What glows (lava, #GLOW, #EMISSIVE), this scene's: its box (its model's, in its own
    // space) and where it is, for the lights by it (GatherLights: a lamp's, not a fill's)
    struct Emitter {
        mat4 transform, inverse;
        vec3 min, max;
    };
    std::vector<Emitter> emitters;
    int current = 0;
    int width = 0, height = 0;
    bool ready = false;
    uint32_t frame = 0;
    // Last frame's trace, if there was one: where its history goes on from
    bool have_prev = false;
    uint64_t prev_presented = 0;
    mat4 prev_proj_view;
    vec3 prev_camera;
    // NRD's: last frame's unjittered world-to-clip, view and (NRD's, rows flipped)
    // projection; whether NRD denoised it
    mat4 prev_unjittered, prev_view, prev_nrd_proj;
    bool nrd_last = false;
    // The level's light probes as a regular grid for GI's hits (VkRt::LightingPass::
    // probe_grid): the colour pass's ambient light at any point, as LightProbeCollection
    // interpolates it, sampled at the grid's corners. Built a slice of points a frame once
    // the probes are done baking, again when they change
    struct ProbeGrid {
        std::vector<uint32_t> data;      // (the finished one's points, 12 words each)
        uint64_t version = 0;
        int data_dims[3] = {0, 0, 0};
        float data_min[3] = {}, data_inv[3] = {};
        std::vector<uint32_t> building;  // (the next one, part done)
        uint64_t source = 0;             // (what the probes were when it was started)
        int dims[3] = {0, 0, 0};
        vec3 min, cell;
        int next = 0, guess = -1;
        bool active = false;
    } probes;
    // What GI's hits read of the game's ambient light (VkRt::LightingPass::probe_grid): a
    // header of 16 words (the grid's points a side and 1 if there is one; its first point;
    // 1 / its step; the shadow decals' count and first word), the grid's points, then the
    // level's shadow decals (16 words: world to the decal's box (3 rows of 4), its kind,
    // its tint's first channel). A new version when either changes
    std::vector<uint32_t> ambient;
    uint64_t ambient_version = 0;
    uint64_t ambient_grid = ~0ull, ambient_decals = 0;
};
State s;

TextureRef MakeTarget(int width, int height, GLint format, GLenum channels, const char* name) {
    vkgl::NextTextureWritable();  // (the passes write it)
    TextureRef t = Textures::Instance()->makeRectangularTexture(width, height, format, channels);
    Textures::Instance()->SetTextureName(t, name);
    return t;
}

// An object's colour texture (an alpha-tested one's alpha cuts it out; reflections see it), 0: none
GLuint ColorTexture(EnvObject* eo) {
    if (eo->texture_ref_.empty() || !eo->texture_ref_[0].valid()) {
        return 0;
    }
    return Textures::Instance()->returnTexture(eo->texture_ref_[0]);  // (VkRt leaves an alpha-tested one out until it's loaded)
}

// How far an object's texture moves down a second (envobject.vert #SCROLL_*: water)
float Scroll(const std::string& shader) {
    if (shader.find("#SCROLL_VERY_SLOW") != std::string::npos) {
        return 0.2f;
    }
    if (shader.find("#SCROLL_SLOW") != std::string::npos) {
        return 0.3f;
    }
    if (shader.find("#SCROLL_FAST") != std::string::npos) {
        return 3.0f;
    }
    if (shader.find("#SCROLL_MEDIUM") != std::string::npos) {
        return 1.0f;
    }
    return 0.0f;
}

// A plant as its draw places and moves it (EnvObject's batch: the bend a push gives it in
// its rotation, its shake in the colour tint's alpha; envobject.vert #PLANT the wind)
void AddPlant(EnvObject* eo, const Model& model, const VkRt::Surface& surface, const vec3& camera_pos, float plant_range) {
    PlantComponent* plant = eo->plant_component_.get();
    quaternion rotation = eo->GetRotation();
    float shake = 0.0f;
    if (plant && plant->IsActive()) {
        rotation = plant->GetQuaternion(eo->sphere_radius_) * eo->GetRotation();
    }
    if (plant) {
        shake = plant->GetPlantShake(eo->sphere_radius_);
    }
    if (eo->attached_ != NULL) {
        shake = 0.0f;  // (its draw's tint alpha -1: no shake)
    }
    mat4 translation_mat;
    translation_mat.SetTranslation(eo->GetTranslation());
    mat4 scale_mat;
    scale_mat.SetScale(eo->GetScale());
    const mat4 transform = translation_mat * Mat4FromQuaternion(rotation) * scale_mat;
    VkRt::PlantPose pose;
    memcpy(pose.transform, transform.entries, sizeof(pose.transform));
    const vec3 scale = eo->GetScale(), translation = eo->GetTranslation();
    for (int i = 0; i < 3; ++i) {
        pose.scale[i] = scale[i];
        pose.translation[i] = translation[i];
    }
    for (int i = 0; i < 4; ++i) {
        pose.rotation[i] = rotation.entries[i];
    }
    pose.time = game_timer.GetRenderTime();
    pose.shake = shake;
    pose.scroll = surface.scroll;
    pose.waterfall = surface.waterfall;
    // (moved where the camera is near: farther, the wind's flutter is under a pixel)
    const bool deform = distance(camera_pos, eo->sphere_center_) - eo->sphere_radius_ < plant_range;
    VkRt::AddPlant(eo, model, pose, surface, deform);
}

// What the colour pass draws: static meshes that aren't transparent or invisible, the
// terrain, items, characters (skinned as their draw does)
// A model's box in its own space (its vertices'), worked out once
void ModelBox(int model_id, const Model& model, vec3* min, vec3* max) {
    static std::unordered_map<int, std::pair<vec3, vec3> > boxes;
    auto it = boxes.find(model_id);
    if (it == boxes.end()) {
        vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], model.vertices[i + k]);
                hi[k] = std::max(hi[k], model.vertices[i + k]);
            }
        }
        it = boxes.emplace(model_id, std::make_pair(lo, hi)).first;
    }
    *min = it->second.first;
    *max = it->second.second;
}

// What an object file's shader name says about its surface, read once per object file
// (the gather walks every static mesh each frame: no string searches there)
struct ShaderTraits {
    std::string shader;  // (the name it was read from: one that changed is read again)
    bool invisible = false, alpha = false, metalness_pbr = false, plant = false, waterfall = false;
    float scroll = 0.0f, emission = 0.0f;
};
const ShaderTraits& Traits(const ObjectFile* file) {
    static std::unordered_map<const ObjectFile*, ShaderTraits> cache;
    ShaderTraits& t = cache[file];
    if (t.shader.empty() || t.shader != file->shader_name) {
        const std::string& shader = file->shader_name;
        t.shader = shader;
        t.invisible = shader.find("#INVISIBLE") != std::string::npos;
        t.alpha = shader.find("#ALPHA") != std::string::npos;
        t.metalness_pbr = shader.find("#METALNESS_PBR") != std::string::npos;
        t.plant = shader.find("#PLANT") != std::string::npos;
        t.waterfall = shader.find("#WATERFALL") != std::string::npos;
        t.scroll = Scroll(shader);
        // (what glows: its colour is its emission, the point lights around it its light; as
        // bright as envobject.frag draws it: #MAGMA_FLOOR its colour * 16 by a mask and a
        // noise about a quarter on average, #MAGMA_FLOW * 2 by a mask about 0.4, #GLOW * 5,
        // #EMISSIVE * 1)
        t.emission = shader.find("#MAGMA_FLOOR") != std::string::npos ? 4.0f
                     : shader.find("#MAGMA_FLOW") != std::string::npos ? 0.8f
                     : shader.find("#GLOW") != std::string::npos       ? 5.0f
                     : shader.find("#EMISSIVE") != std::string::npos   ? 1.0f
                                                                       : 0.0f;
    }
    return t;
}

void GatherScene(SceneGraph* scenegraph, const vec3& camera_pos) {
    const float plant_range = std::max(0.0f, config["rt_plant_range"].toNumber<float>());
    VkRt::BeginScene();
    s.emitters.clear();
    Models* models = Models::Instance();
    for (EnvObject* eo : scenegraph->visible_static_meshes_) {
        // (as the colour pass draws them: not transparent, and not #INVISIBLE, the invisible
        // walls and blockers its shader discards whole)
        if (!eo->enabled_ || eo->transparent || !eo->ofr.valid()) {
            continue;
        }
        const ShaderTraits& traits = Traits(eo->ofr.operator->());
        if (traits.invisible) {
            continue;
        }
        VkRt::Surface surface;
        surface.texture = ColorTexture(eo);
        surface.alpha_tested = traits.alpha;
        if (surface.alpha_tested && surface.texture == 0) {
            continue;  // (an alpha-tested one whose texture isn't there yet: later)
        }
        surface.scroll = traits.scroll;
        surface.double_sided = eo->ofr->double_sided;
        surface.waterfall = traits.waterfall;
        // (its tint as its draw tints it: by its normal map's alpha, the whole of it for
        // #METALNESS_PBR, none for an alpha-tested one)
        if (!surface.alpha_tested) {
            const vec3 tint = eo->RtDisplayTint();
            surface.tint[0] = tint[0];
            surface.tint[1] = tint[1];
            surface.tint[2] = tint[2];
            if (!traits.metalness_pbr && !eo->normal_texture_ref_.empty() && eo->normal_texture_ref_[0].valid()) {
                surface.tint_mask = Textures::Instance()->returnTexture(eo->normal_texture_ref_[0]);
            }
        }
        surface.emission = traits.emission;
        surface.emitter = surface.emission > 0.0f;
        const Model& model = models->GetModel(eo->model_id_);
        if (surface.emitter) {
            State::Emitter e;
            e.transform = eo->GetTransform();
            e.inverse = invert(e.transform);
            ModelBox(eo->model_id_, model, &e.min, &e.max);
            s.emitters.push_back(e);
        }
        if (traits.plant || traits.waterfall) {
            AddPlant(eo, model, surface, camera_pos, plant_range);  // (moving: the wind's shake)
        } else {
            VkRt::AddMesh(model, eo->GetTransform().entries, surface);
        }
    }
    const mat4 identity;
    for (Object* obj : scenegraph->objects_) {
        if (!obj->enabled_) {
            continue;
        }
        if (obj->GetType() == _terrain_type) {
            TerrainObject* to = static_cast<TerrainObject*>(obj);
            const Model* model = to->GetModel();
            if (model) {
                VkRt::Surface surface;
                if (to->terrain_.color_texture_ref.valid()) {
                    surface.texture = Textures::Instance()->returnTexture(to->terrain_.color_texture_ref);
                }
                VkRt::AddMesh(*model, identity.entries, surface);  // (in world space already)
            }
        } else if (obj->GetType() == _item_object) {
            ItemObject* io = static_cast<ItemObject*>(obj);
            const mat4 transform = io->GetTransform();  // (where it was last drawn: the prepass, if it's in view)
            VkRt::Surface surface;
            if (io->ColorTextureRef().valid()) {
                surface.texture = Textures::Instance()->returnTexture(io->ColorTextureRef());
            }
            VkRt::AddMesh(models->GetModel(io->model_id()), transform.entries, surface);
        }
    }
    // Grass and the other detail objects (rt_grass): the instances this frame draws, within
    // rt_grass_range, as alpha-tested cards that shadow, occlude and bounce light. Each sunk
    // into the ground as its draw sinks it (envobject.vert: by its embedding, or more as it
    // fades out with the distance); the wind's sway left out
    if (config["rt_grass"].toNumber<bool>()) {
        const float range = std::max(0.0f, config["rt_grass_range"].toNumber<float>());
        const size_t most = (size_t)std::max(0, config["rt_grass_max"].toNumber<int>());
        Camera* cam = ActiveCameras::Get();
        const float fov_scale = cam ? std::min(3.0f, 90.0f / cam->GetFOV()) : 1.0f;
        size_t added = 0;
        auto add_surface = [&](DetailObjectSurface* dos) {
            const std::vector<mat4>& transforms = dos->DrawnTransforms();
            const std::vector<DetailInstance>& instances = dos->DrawnInstances();
            const TextureRef color = dos->ColorTexture();
            if (transforms.empty() || transforms.size() != instances.size() || !color.valid() || dos->DetailModelId() < 0 || added >= most) {
                return;
            }
            const float max_distance = std::max(0.01f, dos->ViewDist() * fov_scale);
            const float height = dos->BatchHeight();
            s.grass.clear();
            for (size_t i = 0; i < transforms.size() && added + s.grass.size() / 16 < most; ++i) {
                const mat4& t = transforms[i];
                const vec3 origin(t.entries[12], t.entries[13], t.entries[14]);
                const float d = distance(origin, camera_pos);
                if (d > range) {
                    continue;
                }
                const float sink = std::max(instances[i].embed, d / max_distance) * height * instances[i].height_scale;
                const size_t at = s.grass.size();
                s.grass.insert(s.grass.end(), t.entries, t.entries + 16);
                s.grass[at + 13] -= sink;
            }
            if (s.grass.empty()) {
                return;
            }
            VkRt::Surface surface;
            surface.texture = Textures::Instance()->returnTexture(color);
            surface.alpha_tested = true;
            surface.double_sided = true;
            surface.grass = true;
            VkRt::AddMeshInstances(models->GetModel(dos->DetailModelId()), s.grass.data(), s.grass.size() / 16, surface);
            added += s.grass.size() / 16;
        };
        for (Object* obj : scenegraph->objects_) {
            if (!obj->enabled_) {
                continue;
            }
            if (obj->GetType() == _terrain_type) {
                for (DetailObjectSurface* dos : static_cast<TerrainObject*>(obj)->terrain_.detail_object_surfaces) {
                    add_surface(dos);
                }
            }
        }
        for (EnvObject* eo : scenegraph->visible_static_meshes_) {
            if (eo->enabled_ && eo->HasDetailObjectSurfaces()) {
                for (DetailObjectSurface* dos : eo->DetailObjectSurfaces()) {
                    add_surface(dos);
                }
            }
        }
        s.grass_count = added;
    }
    for (Object* obj : scenegraph->movement_objects_) {
        RiggedObject* ro = obj->enabled_ ? static_cast<MovementObject*>(obj)->rigged_object() : NULL;
        if (ro == NULL) {
            continue;
        }
        // (the bones its draw skins with: another machine's stream for a proxy)
        const std::vector<mat4>& bones = ro->network_proxy && ro->network_display_bone_matrices.size() == ro->display_bone_matrices.size()
                                             ? ro->network_display_bone_matrices
                                             : ro->display_bone_matrices;
        const int lod = ro->lod_level >= 0 && ro->lod_level < 4 && ro->model_id[ro->lod_level] != -1 ? ro->lod_level : 0;
        if (bones.empty() || ro->model_id[lod] == -1) {
            continue;
        }
        const GLuint texture = ro->texture_ref.valid() ? Textures::Instance()->returnTexture(ro->texture_ref) : 0;
        VkRt::AddSkinnedMesh(ro, models->GetModel(ro->model_id[lod]), &bones[0].entries[0], bones.size(), texture);
    }
}

// The point lights as the colour pass has them (SceneGraph's ShaderLight: its light reaches
// half the radius), those whose reach is in view; the nearest kMaxLights if there are more
void GatherLights(SceneGraph* scenegraph, const mat4& proj_view, const vec3& camera_pos) {
    const size_t kMaxLights = 256;
    s.lights.clear();
    // (the view's sides and its near plane, from the matrix's rows; the far plane is at infinity)
    float planes[5][4];
    for (int i = 0; i < 4; ++i) {
        const float w = proj_view.entries[i * 4 + 3];
        planes[0][i] = w + proj_view.entries[i * 4 + 0];
        planes[1][i] = w - proj_view.entries[i * 4 + 0];
        planes[2][i] = w + proj_view.entries[i * 4 + 1];
        planes[3][i] = w - proj_view.entries[i * 4 + 1];
        planes[4][i] = w - proj_view.entries[i * 4 + 2];
    }
    for (float* plane : planes) {
        const float len = sqrtf(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (len > 1e-12f) {
            for (int i = 0; i < 4; ++i) {
                plane[i] /= len;
            }
        }
    }
    struct Near {
        float distance;
        size_t index;
    };
    std::vector<Near> near;
    // The level's own lights (saved with it, placed by its designers) are often fill lights
    // with nothing giving them off, there to fake the light that bounces: hard shadows from
    // one read as light pouring out of a hole. Their shadows aren't traced unless something
    // that glows is by them (lava under a lava light: a lamp's) or they're a flame's colour;
    // those a script makes as it goes (fires, torches, lanterns) always are;
    // rt_light_shadows_fill 1: all are. Marked by the sign of their weight (below)
    static std::vector<int> fill_ids;
    fill_ids.clear();
    if (!config["rt_light_shadows_fill"].toNumber<bool>()) {
        for (Object* obj : scenegraph->objects_) {
            if (obj->GetType() == _dynamic_light_object && !obj->exclude_from_save) {
                fill_ids.push_back(static_cast<DynamicLightObject*>(obj)->LightId());
            }
        }
    }
    const std::vector<DynamicLight>& lights = scenegraph->dynamic_light_collection.GetLights();
    for (size_t i = 0; i < lights.size(); ++i) {
        const DynamicLight& dl = lights[i];
        const float reach = dl.radius / 2.0f;
        const float luminance = 0.2126f * dl.color[0] + 0.7152f * dl.color[1] + 0.0722f * dl.color[2];
        if (reach <= 0.8f || luminance <= 0.0f) {
            continue;  // (none of its light gets anywhere: CalculateLightContrib's falloff is 0 past 0.8)
        }
        bool in_view = true;
        for (const float* plane : planes) {
            if (plane[0] * dl.pos[0] + plane[1] * dl.pos[1] + plane[2] * dl.pos[2] + plane[3] < -reach) {
                in_view = false;
                break;
            }
        }
        if (in_view) {
            near.push_back({distance(dl.pos, camera_pos) - reach, i});
        }
    }
    if (near.size() > kMaxLights) {
        std::nth_element(near.begin(), near.begin() + kMaxLights, near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
        near.resize(kMaxLights);
    }
    for (const Near& n : near) {
        const DynamicLight& dl = lights[n.index];
        // (a flame's colour, orange with no blue in it, is a fire's or a torch's however it was
        // placed: the fills fake the sky's light, white or blue)
        const bool flame = dl.color[2] <= 0.1f * std::max(dl.color[0], dl.color[1]) && dl.color[0] >= dl.color[1] && dl.color[0] > 0.0f;
        bool fill = !flame && std::find(fill_ids.begin(), fill_ids.end(), dl.id) != fill_ids.end();
        for (size_t e = 0; fill && e < s.emitters.size(); ++e) {
            // (the nearest point of its box, in the world: within 3 units, it's this light's)
            const State::Emitter& em = s.emitters[e];
            const vec3 local = em.inverse * dl.pos;
            vec3 clamped;
            for (int k = 0; k < 3; ++k) {
                clamped[k] = std::max(em.min[k], std::min(em.max[k], local[k]));
            }
            fill = distance(em.transform * clamped, dl.pos) > 3.0f;
        }
        const float luminance = 0.2126f * dl.color[0] + 0.7152f * dl.color[1] + 0.0722f * dl.color[2];
        const float entry[8] = {dl.pos[0], dl.pos[1], dl.pos[2], dl.radius / 2.0f, dl.color[0], dl.color[1], dl.color[2],
                                fill ? -luminance : luminance};  // (negative: a fill light, its shadow not traced)
        s.lights.insert(s.lights.end(), entry, entry + 8);
    }
}

// An IEEE half's bits (round to nearest; the probes' colours: no NaNs, nothing tiny matters)
uint32_t HalfBits(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = (int)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0) {
        return sign;  // (too small: zero)
    }
    if (exponent >= 31) {
        return sign | 0x7BFFu;  // (too big: the largest)
    }
    mantissa += 0x1000u;  // (rounding)
    if (mantissa & 0x800000u) {
        mantissa = 0;
        if (exponent + 1 >= 31) {
            return sign | 0x7BFFu;
        }
        return sign | ((uint32_t)(exponent + 1) << 10);
    }
    return sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
}

// The level's light probes, as GI's grid (State::ProbeGrid): what they are now (none:
// 0), started over when that changes and the probes aren't baking; a slice of points a
// frame; the finished grid's version bumped
void UpdateProbeGrid(SceneGraph* scenegraph) {
    State::ProbeGrid& g = s.probes;
    LightProbeCollection& lpc = scenegraph->light_probe_collection;
    // (what they are: their count, places and colours, hashed)
    uint64_t source = 0;
    if (!lpc.tet_mesh.tet_points.empty() && lpc.probe_lighting_enabled) {
        source = 1469598103934665603ull;
        auto mix = [&source](float v) {
            uint32_t bits;
            memcpy(&bits, &v, sizeof(bits));
            source = (source ^ bits) * 1099511628211ull;
        };
        for (const LightProbe& probe : lpc.light_probes) {
            for (int k = 0; k < 3; ++k) {
                mix(probe.pos[k]);
            }
            for (int f = 0; f < 6; ++f) {
                for (int k = 0; k < 3; ++k) {
                    mix(probe.ambient_cube_color[f][k]);
                }
            }
            mix(probe.negative ? 1.0f : 0.0f);
        }
        mix((float)lpc.tet_mesh.tet_points.size());
        source |= 1u;
    }
    const bool baking = !lpc.to_process.empty();
    if (source != g.source && !baking) {
        g.source = source;
        g.active = false;
        g.building.clear();
        if (source == 0) {
            if (!g.data.empty()) {
                g.data.clear();
                ++g.version;
            }
            return;
        }
        // (the probes' bounds, a point every metre or more: at most 160 a side and about
        // 300k in all)
        const vec3 lo = lpc.grid_lookup.bounds[0], hi = lpc.grid_lookup.bounds[1];
        const vec3 extent = hi - lo;
        if (!(extent[0] > 0.0f && extent[1] > 0.0f && extent[2] > 0.0f)) {
            return;
        }
        float step = std::max(1.0f, std::cbrt(extent[0] * extent[1] * extent[2] / 300000.0f));
        for (int k = 0; k < 3; ++k) {
            step = std::max(step, extent[k] / 159.0f);
        }
        for (int k = 0; k < 3; ++k) {
            g.dims[k] = std::max(2, (int)std::ceil(extent[k] / step) + 1);
        }
        g.min = lo;
        g.cell = vec3(step);
        const size_t points = (size_t)g.dims[0] * g.dims[1] * g.dims[2];
        g.building.assign(points * 12, 0u);
        g.next = 0;
        g.guess = -1;
        g.active = true;
    }
    if (!g.active) {
        return;
    }
    // (a slice: points x fastest; each starts its tetrahedron walk from the last one's)
    const int points = g.dims[0] * g.dims[1] * g.dims[2];
    const int end = std::min(points, g.next + 6144);
    for (; g.next < end; ++g.next) {
        const int x = g.next % g.dims[0], y = (g.next / g.dims[0]) % g.dims[1], z = g.next / (g.dims[0] * g.dims[1]);
        const vec3 pos = g.min + vec3((float)x, (float)y, (float)z) * g.cell;
        vec3 cube[6];
        for (int f = 0; f < 6; ++f) {
            cube[f] = vec3(-1.0f);  // (GetTetrahedron can return without filling it: unset)
        }
        const int tet = lpc.GetTetrahedron(pos, cube, g.guess);
        uint32_t* out = &g.building[(size_t)g.next * 12];
        if (tet < 0 || cube[0][0] < 0.0f) {
            continue;  // (outside the probes: all zero, not valid)
        }
        g.guess = tet;
        for (int f = 0; f < 6; ++f) {
            out[f * 2] = HalfBits(cube[f][0]) | (HalfBits(cube[f][1]) << 16);
            out[f * 2 + 1] = HalfBits(cube[f][2]) | (HalfBits(f == 0 ? 1.0f : 0.0f) << 16);  // (face 0's: valid)
        }
    }
    if (g.next >= points) {
        for (int k = 0; k < 3; ++k) {
            g.data_dims[k] = g.dims[k];
            g.data_min[k] = g.min[k];
            g.data_inv[k] = 1.0f / g.cell[k];
        }
        g.data.swap(g.building);
        g.building.clear();
        g.active = false;
        ++g.version;
        LOGI << "Ray tracing: GI's light probe grid " << g.dims[0] << "x" << g.dims[1] << "x" << g.dims[2] << " (" << lpc.light_probes.size()
             << " probes)" << std::endl;
    }
}

// What GI's hits read of the game's ambient light (State::ambient): the probes' grid and
// the level's shadow decals (the colour pass's ambient_mult and env_ambient_mult: what
// keeps a cave dark), made again when either changes
void UpdateGameAmbient(SceneGraph* scenegraph) {
    UpdateProbeGrid(scenegraph);
    // (the shadow decals: kinds 1, 7 and 13, as decals.glsl's decalShadow, decalEnvShadow
    // and decalEnvShadowSquare)
    std::vector<uint32_t> decals;
    uint64_t decal_hash = 1469598103934665603ull;
    for (Object* o : scenegraph->decal_objects_) {
        DecalObject* d = static_cast<DecalObject*>(o);
        if (!d->enabled_ || !d->decal_file_ref.valid()) {
            continue;
        }
        const int kind = d->decal_file_ref->special_type;
        if (kind != 1 && kind != 7 && kind != 13) {
            continue;
        }
        const mat4 to_box = invert(d->GetTransform());
        float entry[16];
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 4; ++c) {
                entry[row * 4 + c] = to_box.entries[c * 4 + row];
            }
        }
        entry[12] = (float)kind;
        entry[13] = d->color_tint_component_.tint_[0] * (1.0f + d->color_tint_component_.overbright_ * 0.3f);
        entry[14] = entry[15] = 0.0f;
        const size_t at = decals.size();
        decals.resize(at + 16);
        memcpy(&decals[at], entry, sizeof(entry));
        for (int i = 0; i < 16; ++i) {
            decal_hash = (decal_hash ^ decals[at + i]) * 1099511628211ull;
        }
        if (decals.size() >= 256 * 16) {
            break;
        }
    }
    const State::ProbeGrid& g = s.probes;
    if (g.version == s.ambient_grid && decal_hash == s.ambient_decals && !s.ambient.empty()) {
        return;
    }
    s.ambient_grid = g.version;
    s.ambient_decals = decal_hash;
    const bool grid = !g.data.empty();
    s.ambient.assign(16, 0u);
    if (grid) {
        for (int k = 0; k < 3; ++k) {
            s.ambient[k] = (uint32_t)g.data_dims[k];
            memcpy(&s.ambient[4 + k], &g.data_min[k], sizeof(float));
            memcpy(&s.ambient[8 + k], &g.data_inv[k], sizeof(float));
        }
        s.ambient[3] = 1u;
        s.ambient.insert(s.ambient.end(), g.data.begin(), g.data.end());
    }
    s.ambient[12] = (uint32_t)(decals.size() / 16);
    s.ambient[13] = (uint32_t)s.ambient.size();
    s.ambient.insert(s.ambient.end(), decals.begin(), decals.end());
    ++s.ambient_version;
}

}  // namespace

bool GrassInPrepass() {
    return config["rt_grass"].toNumber<bool>() && Wanted();
}

float GrassRange() {
    return std::max(0.0f, config["rt_grass_range"].toNumber<float>());
}

bool Wanted() {
    return UseVulkan() &&
           (config["rt_shadows"].toNumber<bool>() || config["rt_ao"].toNumber<bool>() || config["rt_light_shadows"].toNumber<bool>() ||
            config["rt_reflections"].toNumber<bool>() || config["rt_gi"].toNumber<bool>()) &&
           VkRt::Available();
}

void TraceLighting(SceneGraph* scenegraph, Camera* camera, const mat4& proj, const mat4& view, int width, int height) {
    s.ready = false;
    s.gi_ready = false;
    if (!Wanted() || width <= 0 || height <= 0) {
        s.have_prev = false;
        return;
    }
    Textures* textures = Textures::Instance();
    if (!s.mask.valid() || s.width != width || s.height != height) {
        for (int i = 0; i < 2; ++i) {
            s.raw[i] = MakeTarget(width, height, GL_RGBA32F, GL_RGBA, "RT::Raw");
            s.history[i] = MakeTarget(width, height, GL_RGBA16F, GL_RGBA, "RT::History");
        }
        s.mask = MakeTarget(width, height, GL_RGBA32F, GL_RGBA, "RT::Lighting");
        s.extra = MakeTarget(width, height, GL_RG16F, GL_RG, "RT::Penumbra");
        s.gi_raw = TextureRef();
        s.gi_dir = TextureRef();
        s.gi_sh0 = TextureRef();
        s.gi_sh1 = TextureRef();
        s.width = width;
        s.height = height;
        s.have_prev = false;
    }
    PROFILER_GPU_ZONE(g_profiler_ctx, "RT lighting");
    const mat4 proj_view = proj * view;
    const vec3 camera_pos = camera->GetPos();
    GatherScene(scenegraph, camera_pos);
    {
        // (where the camera is, now and then: a player's log says where to look)
        static float logged_at = -1000.0f;
        const float now = game_timer.GetWallTime();
        if (now - logged_at > 30.0f) {
            logged_at = now;
            LOGI << "Ray tracing: camera at " << camera_pos[0] << ", " << camera_pos[1] << ", " << camera_pos[2] << " looking "
                 << camera->GetFacing()[0] << ", " << camera->GetFacing()[1] << ", " << camera->GetFacing()[2] << std::endl;
        }
    }
    // Last frame's history goes on where last frame was traced too, from near here (a
    // camera cut starts it over)
    const uint64_t presented = VkBackend::FramesPresented();
    const bool has_history = s.have_prev && presented == s.prev_presented + 1 && distance(camera_pos, s.prev_camera) < 3.0f;
    s.current ^= 1;
    VkRt::LightingPass p;
    Graphics* graphics = Graphics::Instance();
    p.depth = textures->returnTexture(graphics->multisample_framebuffer_exists ? graphics->multisample_depth_tex : graphics->screen_depth_tex);
    p.raw = textures->returnTexture(s.raw[s.current]);
    p.raw_prev = textures->returnTexture(s.raw[s.current ^ 1]);
    p.history = textures->returnTexture(s.history[s.current]);
    p.history_prev = textures->returnTexture(s.history[s.current ^ 1]);
    p.mask = textures->returnTexture(s.mask);
    p.extra = textures->returnTexture(s.extra);
    p.width = (uint32_t)width;
    p.height = (uint32_t)height;
    const mat4 inv = invert(proj_view);
    memcpy(p.inv_view_proj, inv.entries, sizeof(p.inv_view_proj));
    p.has_history = has_history;
    if (has_history) {
        const mat4 reprojection = s.prev_proj_view * inv;
        memcpy(p.reprojection, reprojection.entries, sizeof(p.reprojection));
    }
    p.denoise = config["rt_denoise"].toNumber<bool>();
    p.camera[0] = camera_pos[0];
    p.camera[1] = camera_pos[1];
    p.camera[2] = camera_pos[2];
    p.near_plane = camera->GetNearPlane();
    p.pixel_angle = 2.0f / (std::max(1e-6f, proj.entries[5]) * (float)height);  // (2 tan(fov / 2) over the height)
    const vec3 sun = normalize(scenegraph->primary_light.pos);
    p.sun[0] = sun[0];
    p.sun[1] = sun[1];
    p.sun[2] = sun[2];
    // (the sun's disk, its angular diameter in degrees: the real one is about 0.53)
    const float sun_size = std::max(0.0f, std::min(10.0f, config["rt_sun_size"].toNumber<float>()));
    p.cone = sun_size * 0.5f * 3.14159265f / 180.0f;
    p.sun_rays = config["rt_shadows"].toNumber<bool>() ? std::max(1, std::min(64, config["rt_shadow_rays"].toNumber<int>())) : 0;
    p.ao_rays = config["rt_ao"].toNumber<bool>() ? std::max(1, std::min(64, config["rt_ao_rays"].toNumber<int>())) : 0;
    p.ao_range = std::max(0.05f, std::min(20.0f, config["rt_ao_range"].toNumber<float>()));
    p.frame = s.frame++;
    p.bias = std::max(0.0f, config["rt_shadow_bias"].toNumber<float>());
    p.time = game_timer.GetRenderTime();
    // NVIDIA's denoisers (NRD) where they're there: the camera unjittered (the motion), the
    // matrices NRD takes, its rows flipped so its first image row is ours (the bottom)
    p.nrd = p.denoise && config["rt_nrd"].toNumber<bool>() && VkRt::NrdAvailable();
    const mat4 unjittered = Temporal::InScene() ? Temporal::ViewProj() : proj_view;
    mat4 nrd_proj = unjittered * invert(view);
    for (int c = 0; c < 4; ++c) {
        nrd_proj.entries[c * 4 + 1] = -nrd_proj.entries[c * 4 + 1];
    }
    if (p.nrd) {
        p.sun_rays = std::min(p.sun_rays, 1);  // (SIGMA: one ray a pixel)
        const bool go_on = has_history && s.nrd_last;
        p.nrd_reset = !go_on;
        memcpy(p.view_proj, unjittered.entries, sizeof(p.view_proj));
        memcpy(p.prev_view_proj, (go_on ? s.prev_unjittered : unjittered).entries, sizeof(p.prev_view_proj));
        memcpy(p.nrd_view, view.entries, sizeof(p.nrd_view));
        memcpy(p.nrd_proj, nrd_proj.entries, sizeof(p.nrd_proj));
        memcpy(p.nrd_view_prev, (go_on ? s.prev_view : view).entries, sizeof(p.nrd_view_prev));
        memcpy(p.nrd_proj_prev, (go_on ? s.prev_nrd_proj : nrd_proj).entries, sizeof(p.nrd_proj_prev));
    }
    // Global illumination (NRD denoises it: no NRD, none): the sky's cube map for the rays
    // that meet nothing, the sun's light as the colour pass has it
    // (or DLSS Ray Reconstruction denoises it, in the frame)
    p.gi_rr = Temporal::RayReconstruction();
    if ((p.nrd || p.gi_rr) && config["rt_gi"].toNumber<bool>() && scenegraph->sky != NULL) {
        TextureRef sky = scenegraph->sky->GetSpecularCubeMapTexture();
        if (sky.valid()) {
            if (!s.gi_sh0.valid()) {
                s.gi_raw = MakeTarget(width, height, GL_RGBA16F, GL_RGBA, "RT::GiRaw");
                s.gi_dir = MakeTarget(width, height, GL_RGBA16F, GL_RGBA, "RT::GiDirection");
                s.gi_sh0 = MakeTarget(width, height, GL_RGBA16F, GL_RGBA, "RT::GiSh0");
                s.gi_sh1 = MakeTarget(width, height, GL_RGBA16F, GL_RGBA, "RT::GiSh1");
            }
            p.gi = true;
            p.gi_raw = textures->returnTexture(s.gi_raw);
            p.gi_dir = textures->returnTexture(s.gi_dir);
            p.gi_sh0 = textures->returnTexture(s.gi_sh0);
            p.gi_sh1 = textures->returnTexture(s.gi_sh1);
            p.gi_debug = (uint32_t)std::max(0, std::min(5, config["rt_gi_debug"].toNumber<int>()));
            p.sky = textures->returnTexture(sky);
            const vec3 sun_color = scenegraph->primary_light.color * scenegraph->primary_light.intensity;
            p.sun_color[0] = sun_color[0];
            p.sun_color[1] = sun_color[1];
            p.sun_color[2] = sun_color[2];
            const std::string level = scenegraph->level_path_.GetFullPathStr();
            p.gi_cache_reset = level != s.gi_level;
            s.gi_level = level;
            p.gi_voxel = std::max(0.05f, std::min(4.0f, config["rt_gi_voxel"].toNumber<float>()));
            p.gi_emission = std::max(0.0f, std::min(100.0f, config["rt_gi_emission"].toNumber<float>()));
            p.gi_sky_lod = std::max(0.0f, std::min(8.0f, config["rt_gi_sky_lod"].toNumber<float>()));
            p.gi_sky_scale = std::max(0.0f, std::min(10.0f, config["rt_gi_sky"].toNumber<float>()));
            p.gi_lights = std::max(0.0f, std::min(1.0f, config["rt_gi_lights"].toNumber<float>()));
            p.gi_strength = std::max(0.0f, std::min(8.0f, config["rt_gi_strength"].toNumber<float>()));
            p.gi_bounce = std::max(0.0f, std::min(8.0f, config["rt_gi_bounce"].toNumber<float>()));
            p.gi_raster_ambient = std::max(0.0f, std::min(1.0f, config["rt_gi_raster_ambient"].toNumber<float>()));
            // (the hits lit as the colour pass lights them: the probes' ambient there, by
            // GetAmbientContrib(1) for this sun)
            UpdateGameAmbient(scenegraph);
            p.probe_grid = s.ambient.data();
            p.probe_grid_words = s.ambient.size();
            p.probe_grid_version = s.ambient_version;
            p.ambient_contrib = std::max(0.0f, 1.5f - scenegraph->primary_light.intensity * 0.5f);
        }
    }
    // (the point lights: their shadows traced, or what GI's hits are lit by)
    if (config["rt_light_shadows"].toNumber<bool>() || p.gi) {
        GatherLights(scenegraph, proj_view, camera_pos);
        if (config["rt_log_lights"].toNumber<bool>()) {  // (diagnostics: the lights this frame uses, once)
            static bool logged = false;
            if (!logged && !s.lights.empty()) {
                logged = true;
                for (size_t i = 0; i + 8 <= s.lights.size(); i += 8) {
                    LOGI << "Ray tracing: light at " << s.lights[i] << ", " << s.lights[i + 1] << ", " << s.lights[i + 2] << " reach " << s.lights[i + 3]
                         << " colour " << s.lights[i + 4] << ", " << s.lights[i + 5] << ", " << s.lights[i + 6]
                         << (s.lights[i + 7] < 0.0f ? " (fill: no shadow)" : " (shadowed)") << std::endl;
                }
            }
        }
        p.lights = s.lights.data();
        p.light_count = (uint32_t)(s.lights.size() / 8);
        p.light_rays = config["rt_light_shadows"].toNumber<bool>() ? std::max(1, std::min(16, config["rt_light_rays"].toNumber<int>())) : 0;
        p.light_size = std::max(0.0f, std::min(2.0f, config["rt_light_size"].toNumber<float>()));
        p.light_clearance = std::max(0.0f, std::min(2.5f, config["rt_light_clearance"].toNumber<float>()));
        p.light_cull = !config.HasKey("rt_light_cull") || config["rt_light_cull"].toNumber<bool>();
    }
    s.ready = VkRt::TraceLighting(p);
    s.have_prev = s.ready;
    s.nrd_last = s.ready && p.nrd;
    s.prev_unjittered = unjittered;
    s.prev_view = view;
    s.prev_nrd_proj = nrd_proj;
    s.prev_presented = presented;
    s.prev_proj_view = proj_view;
    s.prev_camera = camera_pos;
    if (s.ready) {
        // (for the whole colour pass: programs changing don't empty its unit)
        textures->bindTexture(s.mask, TEX_RT_LIGHTING);
        textures->PinUnit(TEX_RT_LIGHTING, true);
        s.gi_ready = p.gi && VkRt::GiTraced();
        s.gi_rr = p.gi_rr;
        if (s.gi_ready) {
            textures->bindTexture(p.gi_rr ? s.gi_raw : s.gi_sh0, TEX_RT_GI);
            textures->bindTexture(p.gi_rr ? s.gi_dir : s.gi_sh1, TEX_RT_GI_SH1);
            textures->PinUnit(TEX_RT_GI, true);
            textures->PinUnit(TEX_RT_GI_SH1, true);
        }
    } else if (!p.gi) {
        s.gi_level.clear();  // (not traced: the cache may go stale meanwhile)
    }
}

bool MaskReady() {
    return s.ready;
}

bool GiReady() {
    return s.ready && s.gi_ready;
}

bool ReflectionsReady() {
    VkRt::DrawScene scene;
    return s.ready && config["rt_reflections"].toNumber<bool>() && VkRt::DrawsCanTrace() && VkRt::SceneForDraws(&scene);
}

void EndScene() {
    if (s.ready) {
        Textures::Instance()->PinUnit(TEX_RT_LIGHTING, false);
    }
    if (s.gi_ready) {
        Textures::Instance()->PinUnit(TEX_RT_GI, false);
        Textures::Instance()->PinUnit(TEX_RT_GI_SH1, false);
    }
    s.ready = false;
    s.gi_ready = false;
    VkRt::EndScene();
}

}  // namespace Raytrace
