#include <Internal/config.h>
#include <tut/tut.hpp>

namespace tut {
struct difficulty_presets_data {};
typedef test_group<difficulty_presets_data> difficulty_presets_group;
difficulty_presets_group difficulty_presets_tests("DifficultyPresets");
typedef difficulty_presets_group::object difficulty_test;

template<> template<> void difficulty_test::test<1>() {
    set_test_name("three original presets remain separate from optional scaling");
    Config settings;
    const auto names = settings.GetDifficultyPresets();
    ensure_equals(names.size(), size_t(3));
    ensure_equals(names[2], std::string("Expert"));
    settings.SetDifficultyPreset("Nightmare");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 1.0f);
    ensure_equals(settings["global_time_scale_mult"].toNumber<float>(), 1.0f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Expert"));
    ensure_equals(settings.GetClosestDifficulty(), std::string("Expert"));
    ensure("legacy Nightmare selects scaling", settings["difficulty_scaling"].toBool());
    ensure("legacy Nightmare retains its AI choice", settings["campaign_ai_scaling"].toBool());
    settings.SetDifficultyPreset("Expert");
    ensure("base preset selection preserves independent AI settings", settings["campaign_ai_scaling"].toBool());
    ensure_equals(settings.GetDifficultyPreset(), std::string("Expert"));
    ensure_equals(settings.GetClosestDifficulty(), std::string("Expert"));
}

template<> template<> void difficulty_test::test<2>() {
    set_test_name("base settings preserve original preset classification");
    Config settings;
    settings.SetDifficultyPreset("Nightmare");
    settings.SetDifficultyPreset("Casual");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 0.0f);
    ensure_equals(settings["global_time_scale_mult"].toNumber<float>(), 0.8f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Casual"));
    settings.SetDifficultyPreset("Hardcore");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 0.5f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Hardcore"));
    settings.GetRef("auto_ledge_grab") = true;
    settings.SetDifficultyPreset("Expert", false);
    ensure("match presets preserve local assist", settings["auto_ledge_grab"].toBool());
    ensure_equals(settings.GetDifficultyPreset(false), std::string("Expert"));
    for (const char* key : {"tutorials", "auto_ledge_grab"}) {
        settings.SetDifficultyPreset("Nightmare");
        settings.GetRef(key) = true;
        ensure_equals(settings.GetDifficultyPreset(), std::string("Custom"));
        ensure_equals(settings.GetClosestDifficulty(), std::string("Expert"));
    }
    settings.SetDifficultyPreset("Nightmare");
    settings.GetRef("game_difficulty") = 0.75f;
    ensure_equals(settings.GetClosestDifficulty(), std::string("Hardcore"));
}

template<> template<> void difficulty_test::test<3>() {
    set_test_name("stat and reinforcement preferences remain independent of AI scaling");
    Config settings;
    settings.GetRef("campaign_mp_scaling") = false;
    settings.SetDifficultyPreset("Nightmare");
    ensure("choosing AI does not opt in to stat scaling", !settings["campaign_mp_scaling"].toBool());
    ensure_equals(settings.GetDifficultyPreset(), std::string("Expert"));
    settings.GetRef("campaign_mp_scaling") = true;
    settings.SetDifficultyPreset("Expert");
    ensure("choosing Expert retains stat scaling", settings["campaign_mp_scaling"].toBool());
}
}
