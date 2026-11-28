#pragma once

#include <cstdint>
#include <string>

// Personal controls and settings consumed directly at runtime do not require
// rebuilding render targets or preloading the scene's shaders.
namespace SettingsChanges {
inline bool ScalingMatchSetting(const std::string& key) {
    return key == "campaign_mp_scaling" || key == "campaign_mp_nightmare" || key == "difficulty_scaling" ||
           key == "campaign_ai_scaling_rate" || key == "campaign_ai_scaling" ||
           key == "campaign_armor_scaling" || key == "campaign_weapon_scaling";
}

inline bool CampaignMatchSetting(const std::string& key) {
    return key == "difficulty_preset" || key == "game_difficulty" ||
           key == "global_time_scale_mult" || key == "tutorials" ||
           ScalingMatchSetting(key) || key == "armor_localized" || key == "oo_arena_random";
}

inline bool StartsWith(const std::string& value, const char* prefix) {
    return value.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

inline bool NeedsGraphicsReload(const std::string& key) {
    if (StartsWith(key, "gamepad") || StartsWith(key, "key[") ||
        StartsWith(key, "menu_") || StartsWith(key, "controller_") ||
        StartsWith(key, "test_") || StartsWith(key, "rt_") || StartsWith(key, "oo_")) return false;
    const char* runtime[] = {
        "mouse_sensitivity", "invert_x_mouse_look", "invert_y_mouse_look", "use_raw_input",
        "music_volume", "master_volume", "motion_blur_amount", "brightness",
        "temporal_aa_sharpness", "dlss_sharpness", "fsr_sharpness",
        "difficulty_preset", "difficulty_preset_value", "game_difficulty", "global_time_scale_mult", "tutorials",
        "campaign_mp_scaling", "campaign_mp_nightmare", "difficulty_scaling", "campaign_ai_scaling", "campaign_ai_scaling_rate", "campaign_armor_scaling", "campaign_weapon_scaling", "armor_localized",
        "auto_ledge_grab", "auto_camera", "fps_label", "show_difficulty_readout", "show_stats_readout", "language", "custom_resolution", "last_ip"
    };
    for (const char* value : runtime) if (key == value) return false;
    // Preserve the full reload for unknown/custom settings.
    return true;
}

inline bool SharedClientSetting(const std::string& key) {
    if (StartsWith(key, "rt_")) return true;
    const char* shared[] = {
        "overall", "screenwidth", "screenheight", "fullscreen", "target_monitor", "vsync",
        "limit_fps_in_game", "max_frame_rate", "renderer", "upscaler", "upscaler_quality",
        "multisample", "temporal_aa", "temporal_aa_sharpness", "dlss_sharpness", "fsr_sharpness",
        "brightness", "anisotropy", "texture_reduce", "detail_objects", "detail_object_lowres",
        "detail_object_disable_shadows", "detail_object_decals", "motion_blur_amount",
        "simple_shadows", "no_reflection_capture", "simple_fog", "detail_objects_reduced",
        "simple_water", "custom_level_shaders", "particle_field", "particle_field_simple",
        "depth_of_field", "depth_of_field_reduced", "music_volume", "master_volume",
        "difficulty_preset", "game_difficulty", "global_time_scale_mult", "tutorials",
        "campaign_mp_scaling", "campaign_mp_nightmare", "difficulty_scaling", "campaign_ai_scaling", "campaign_ai_scaling_rate", "campaign_armor_scaling", "campaign_weapon_scaling", "armor_localized",
        "auto_ledge_grab", "fps_label", "language", "blood", "blood_color", "split_screen",
        "auto_camera", "custom_resolution"
    };
    for (const char* value : shared) if (key == value) return true;
    return false;
}

class Pending {
    uint32_t revision_ = 0;
    bool graphics_ = false;
public:
    void Changed(const std::string& key) {
        ++revision_;
        graphics_ = graphics_ || NeedsGraphicsReload(key);
    }
    uint32_t Revision() const { return revision_; }
    bool TakeGraphicsReload() {
        const bool result = graphics_;
        graphics_ = false;
        return result;
    }
};
}
