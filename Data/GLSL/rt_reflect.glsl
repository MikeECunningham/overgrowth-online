//-----------------------------------------------------------------------------
//           Name: rt_reflect.glsl
//      Developer: Overgrowth Online (crxhvrd)
//    Description: Reflections ray traced in the colour pass (envobject.frag #RT_REFLECTIONS:
//                 Vulkan, the scene's TLAS from VkRt::SceneForDraws). A ray along the
//                 reflection (a little spread by the roughness) meets the scene: its
//                 triangle read back from the BLAS (position fetch) for its normal, its
//                 colour texture by the hit's uvs, lit as the colour pass lights things (the
//                 sun, its shadow traced too; the ambient light there, dimmed by the
//                 level's shadow decals there; the point lights), a glowing surface (lava)
//                 by its own colour. Ray traced only: nothing read from the screen.
//                 Nothing met: the captured reflection (the caller's).
//                 Alpha-tested meshes stop rays where their alpha is 0.5 or more.
//                 The shader preprocessor makes a program that includes this GLSL 460 with
//                 the ray query extensions when RT_REFLECTIONS is defined (Shaders)
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#if defined(RT_REFLECTIONS)

// (the engine binds these: vk_compile's AssignBindings, vk_draw)
uniform accelerationStructureEXT rt_scene;
struct RtMaterial {
    uvec2 indices;  // (device addresses)
    uvec2 uvs;      // (0: none)
    uint texture;   // a slot in rt_textures (0xFFFFFFFF: none)
    float scroll;   // how far its texture moves down a second (#SCROLL_*)
    uint flags;     // 1 alpha-tested, 2 emitter, 4 waterfall
    uint tint_mask; // a slot whose alpha masks the tint (0xFFFFFFFF: all of it)
    vec3 tint;      // its colour tint (the editor's: instance_color_tint)
    float emission; // an emitter's light, by its colour (as its draw)
};
layout(std430) readonly buffer RtMaterials {
    RtMaterial rt_materials[];
};
uniform sampler2D rt_textures[];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtIndices {
    uint index[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RtUvs {
    vec2 uv[];
};

vec3 GetFogColorMult();  // (envobject.frag's, further on)
// (envobject.frag's too: the ambient light at a place as the colour pass lights it: the
// level's light volumes, its light probes, else the sky's cube map)
vec3 GetAmbientColor(vec3 world_vert, vec3 ws_normal);

const uint kRtNoTexture = 0xFFFFFFFFu;
const uint kRtAlphaTested = 1u, kRtEmitter = 2u, kRtWaterfall = 4u;

// A hit's uv (as envobject.vert hands its draw the uvs: scrolled, flipped)
vec2 RtHitUv(RtMaterial m, uint primitive, vec2 bary) {
    if (m.uvs == uvec2(0u)) {
        return vec2(0.0);
    }
    RtIndices indices = RtIndices(m.indices);
    RtUvs uvs = RtUvs(m.uvs);
    uint tri = primitive * 3u;
    vec2 uv = uvs.uv[indices.index[tri]] * (1.0 - bary.x - bary.y) + uvs.uv[indices.index[tri + 1u]] * bary.x +
              uvs.uv[indices.index[tri + 2u]] * bary.y;
    uv.y = 1.0 - (uv.y - time * m.scroll);
    return uv;
}

uint RtHash(uint v);

// A candidate on an alpha-tested mesh: solid where its texture is (alpha 0.5 or more, as
// its draw's discard); a waterfall, drawn see-through, as much as its alpha (a different
// cut a pixel and a frame: the temporal pass blends them as its draw blends it)
bool RtAlphaSolid(rayQueryEXT q) {
    RtMaterial m = rt_materials[rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)];
    if (m.texture == kRtNoTexture) {
        return true;
    }
    uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false));
    vec2 uv = RtHitUv(m, primitive, rayQueryGetIntersectionBarycentricsEXT(q, false));
    float cut = 0.5;
    if ((m.flags & kRtWaterfall) != 0u) {
        uint seed = RtHash(uint(gl_FragCoord.x) * 7919u + uint(gl_FragCoord.y) * 104729u + uint(time * 977.0) + primitive * 31u);
        cut = (float(seed & 0xFFFFu) + 0.5) * (1.0 / 65536.0);
    }
    return textureLod(rt_textures[nonuniformEXT(m.texture)], uv, 0.0).a >= cut;
}

// Something between `origin` and t_max along `dir` (the sun's shadow at a hit)
bool RtBlocked(vec3 origin, vec3 dir, float t_max) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, rt_scene, gl_RayFlagsTerminateOnFirstHitEXT, 0xEFu, origin, 0.0, dir, t_max);  // (not grass: a whole field put out a reflection's sun)
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RtAlphaSolid(q)) {
            rayQueryConfirmIntersectionEXT(q);
        }
    }
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// (PCG, Jarzynski and Olano: the spread's directions)
uint RtHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The light a reflection brings back from `pos` (the surface) along `dir`, spread by
// `roughness` (0: a mirror); nothing met: `miss`
vec3 RtReflect(vec3 pos, vec3 normal, vec3 dir, float roughness, vec3 miss);
float RtReflectWeight(float roughness);

// A reflection as the colour pass would look it up (`captured`, the reflection captures),
// traced where the surface is smooth enough, blended into the captured one as it roughens
vec3 RtReflection(vec3 pos, vec3 normal, vec3 dir, float roughness, vec3 captured) {
    float traced = RtReflectWeight(roughness);
    if (traced <= 0.0) {
        return captured;
    }
    return mix(captured, RtReflect(pos, normal, dir, roughness, captured), traced);
}

// The colour pass's ambient dimming at a place (CalculateDecals's: the shadow decals there
// as ambient_mult, the environment shadow decals as env_ambient_mult: what keeps a cave
// dark), from every decal in view (a hit can be off the screen: no cluster of its own)
void RtDecalDimming(vec3 p, out float ambient_dim, out float env_dim) {
    ambient_dim = 1.0;
    env_dim = 1.0;
    #if !defined(NO_DECALS)
        for (uint i = 0u; i < num_decals; ++i) {
            DecalData decal = FetchDecal(i);
            int type = int(decal.tint[3]);
            if (type != decalShadow && type != decalEnvShadow && type != decalEnvShadowSquare) {
                continue;
            }
            vec3 temp = (transpose(mat_from_quat(decal.rotation)) * (p - decal.position)) / decal.scale;
            if (abs(temp.x) < 0.5 && abs(temp.y) < 0.5 && abs(temp.z) < 0.5) {
                if (type == decalShadow) {
                    ambient_dim *= max(0.5, min(1.0, length(temp * 2.0)));
                } else if (type == decalEnvShadow) {
                    env_dim *= mix((max(0.5, min(1.0, length(temp * 2.0))) - 0.5) * 2.0, 1.0, decal.tint[0]);
                } else {
                    env_dim *= mix((clamp(sdRoundBox(temp * 4.0, vec3(0.875), 0.125), 0.5, 1.0) - 0.5) * 2.0, 1.0, decal.tint[0]);
                }
            }
        }
    #endif
}

// The point lights' light at a place (CalculateLightContrib's falloff and facing), from
// every light in view, dimmed as the colour pass dims them there
// (CalculateLightContrib's: a waterfall's facing a constant half, as its draw has it)
vec3 RtLightColor(PointLightData l, vec3 p, vec3 n, bool waterfall) {
    vec3 to_light = l.pos - p;
    float len = length(to_light);
    float dist = max(0.8, len);
    float falloff = min(1.0, max(0.0, (1.0 / dist / dist) * (1.0 - dist / l.radius)));
    if (falloff <= 0.0) {
        return vec3(0.0);
    }
    float bias = max(0.0, (0.8 - len) / 0.8 * 2.0);
    float d = waterfall ? 0.5 : max(0.0, dot(to_light / max(len, 1e-6), n) + bias) / (1.0 + bias);
    return falloff * d * l.color;
}

// (their shadows too, as rt_shadow.comp's light rays: one light picked by its share of the
// light and its ray traced, stopped by the faces turned toward it, not by glowing surfaces;
// what it gives over its chance of being picked. Without it a fire's light went through
// the rock into reflections)
vec3 RtPointLights(vec3 p, vec3 n, vec3 face, float dim, bool waterfall) {
    // (one walk over the lights: a weighted reservoir keeps one, each light in turn taking
    // its place by its share of the light so far: the same pick as by the whole sum, without
    // a second walk that fetches and weighs every light again)
    uint seed = RtHash(uint(gl_FragCoord.x) * 31u + uint(gl_FragCoord.y) * 7177u) ^ RtHash(uint(time * 977.0) + 5u);
    float total = 0.0, picked_share = 0.0;
    vec3 picked_color = vec3(0.0), picked_pos = vec3(0.0);
    for (uint i = 0u; i < num_lights; ++i) {
        PointLightData l = FetchPointLight(i);
        vec3 c = RtLightColor(l, p, n, waterfall);
        float share = dot(c, vec3(0.2126, 0.7152, 0.0722));
        if (share <= 0.0) {
            continue;
        }
        total += share;
        seed = RtHash(seed);
        if (float(seed) * (1.0 / 4294967296.0) * total < share) {
            picked_share = share;
            picked_color = c;
            picked_pos = l.pos;
        }
    }
    if (total <= 0.0 || picked_share <= 0.0) {
        return vec3(0.0);
    }
    vec3 to_light = picked_pos - p;
    vec3 dir = to_light / max(length(to_light), 1e-6);
    vec3 origin = p + face * (dot(face, dir) >= 0.0 ? 0.02 : -0.02);
    float t_max = length(picked_pos - origin) - 0.25;  // (rt_light_clearance's default)
    bool blocked = false;
    if (t_max > 0.0) {
        rayQueryEXT q;
        rayQueryInitializeEXT(q, rt_scene, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullFrontFacingTrianglesEXT, 0x03u, origin, 0.0, dir,
                              t_max);
        while (rayQueryProceedEXT(q)) {
            if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RtAlphaSolid(q)) {
                rayQueryConfirmIntersectionEXT(q);
            }
        }
        blocked = rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT;
    }
    return blocked ? vec3(0.0) : picked_color * (total / picked_share) * dim;
}

vec3 RtReflect(vec3 pos, vec3 normal, vec3 dir, float roughness, vec3 miss) {
    if (roughness > 0.0) {
        // (a direction in a cone round the reflection, as wide as the roughness: a sample
        // a pixel, a frame; the temporal pass averages them)
        uint seed = RtHash(uint(gl_FragCoord.x) + uint(gl_FragCoord.y) * 65536u) ^ RtHash(uint(time * 977.0));
        float u1 = float(RtHash(seed)) * (1.0 / 4294967296.0);
        float u2 = float(RtHash(seed + 1u)) * (1.0 / 4294967296.0);
        vec3 t = normalize(abs(dir.y) < 0.99 ? cross(dir, vec3(0.0, 1.0, 0.0)) : cross(dir, vec3(1.0, 0.0, 0.0)));
        vec3 b = cross(dir, t);
        float r = roughness * roughness * sqrt(u1);
        float a = 6.28318531 * u2;
        vec3 spread = normalize(dir + (t * cos(a) + b * sin(a)) * r);
        dir = dot(spread, normal) > 0.0 ? spread : dir;  // (not into the surface)
    }
    float dist = length(pos - cam_pos);
    vec3 origin = pos + normal * (0.01 + dist * 0.002);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, rt_scene, gl_RayFlagsNoneEXT, 0xFFu, origin, 0.0, dir, 10000.0);
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RtAlphaSolid(q)) {
            rayQueryConfirmIntersectionEXT(q);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        return miss;
    }
    float hit_t = rayQueryGetIntersectionTEXT(q, true);
    vec3 hit = origin + dir * hit_t;
    // The hit's face, in world space (its triangle as the BLAS holds it, placed by its instance)
    vec3 v[3];
    rayQueryGetIntersectionTriangleVertexPositionsEXT(q, true, v);
    mat4x3 to_world = rayQueryGetIntersectionObjectToWorldEXT(q, true);
    vec3 w0 = to_world * vec4(v[0], 1.0), w1 = to_world * vec4(v[1], 1.0), w2 = to_world * vec4(v[2], 1.0);
    vec3 n = cross(w1 - w0, w2 - w0);
    float n_len = length(n);
    n = n_len > 1e-12 ? n / n_len : -dir;
    if (dot(n, dir) > 0.0) {
        n = -n;  // (the side the ray came from)
    }
    RtMaterial m = rt_materials[rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)];
    vec3 albedo = vec3(0.35);
    if (m.texture != kRtNoTexture) {
        vec2 uv = RtHitUv(m, uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)), rayQueryGetIntersectionBarycentricsEXT(q, true));
        // (coarser the farther the hit: a reflection of a far wall doesn't alias)
        float lod = max(0.0, log2(max(hit_t, 1e-3) * 0.5));
        albedo = textureLod(rt_textures[nonuniformEXT(m.texture)], uv, lod).xyz;
        // (tinted as its draw tints it: by its normal map's alpha)
        float tint_amount = m.tint_mask == kRtNoTexture ? 1.0 : textureLod(rt_textures[nonuniformEXT(m.tint_mask)], uv, lod).a;
        albedo *= mix(vec3(1.0), m.tint, tint_amount);
    }
    if ((m.flags & kRtWaterfall) != 0u) {
        albedo = vec3(1.0);  // (a waterfall's draw is its light alone: its texture is only its alpha)
    }
    vec3 radiance;
    if ((m.flags & kRtEmitter) != 0u) {
        radiance = albedo * m.emission;  // (lava and the like: their own light, as bright as their draw)
    } else {
        vec3 l = normalize(ws_light);
        // (lit as the colour pass lights them: a waterfall as though it faced up, half lit;
        // leaves and grass cards from either side)
        vec3 lit_normal = n;
        float lambert = max(0.0, dot(n, l));
        if ((m.flags & kRtWaterfall) != 0u) {
            lit_normal = vec3(0.0, 1.0, 0.0);
            lambert = max(0.0, 0.5 * l.y);
        } else if ((m.flags & kRtAlphaTested) != 0u) {
            lambert = abs(dot(n, l));
        }
        float sun = 0.0;
        if (lambert > 0.0 && primary_light_color.a > 0.0) {
            sun = RtBlocked(hit + n * (0.01 + hit_t * 0.001), l, 10000.0) ? 0.0 : 1.0;
        }
        // (the place's own ambient light, dimmed by its shadow decals: dark in a cave, not
        // the sky's; and the point lights there)
        float ambient_dim, env_dim;
        RtDecalDimming(hit, ambient_dim, env_dim);
        vec3 ambient = GetAmbientColor(hit, lit_normal) * GetAmbientContrib(1.0) * ambient_dim * env_dim;
        vec3 lights = RtPointLights(hit, lit_normal, n, ambient_dim, (m.flags & kRtWaterfall) != 0u);
        radiance = albedo * (GetDirectColor(sun * lambert) * primary_light_color.a + ambient + lights);
    }
    // The level's haze over the way from the camera to the hit, as the colour pass hazes what
    // it draws (its amount by the distance; its colour the ambient light there, looking
    // back: the place's own, a cave's dark): as the screen-space reflection it replaces
    // had it, the camera's side and all
    float haze = GetHazeAmount(pos - cam_pos + dir * hit_t, haze_mult);
    vec3 fog_color = GetAmbientColor(hit, -dir) * GetFogColorMult();
    return mix(radiance, fog_color, haze);
}

// How much of a reflection is traced by its roughness (the game's 0..1, before its 0.9
// remap): all of it up to kRtTracedRoughness * 0.6, none past kRtTracedRoughness (the
// captured reflection there: a ray a pixel is too noisy for rough surfaces without a
// denoiser)
const float kRtTracedRoughness = 0.4;
float RtReflectWeight(float roughness) {
    return 1.0 - smoothstep(kRtTracedRoughness * 0.6, kRtTracedRoughness, roughness);
}

#endif  // RT_REFLECTIONS
