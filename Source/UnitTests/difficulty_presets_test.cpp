#include <Internal/config.h>
#include <tut/tut.hpp>

namespace tut {
struct difficulty_presets_data {};
typedef test_group<difficulty_presets_data> difficulty_presets_group;
difficulty_presets_group difficulty_presets_tests("DifficultyPresets");
typedef difficulty_presets_group::object difficulty_test;

template<> template<> void difficulty_test::test<1>() {
    set_test_name("Nightmare is distinct from Expert without raising the base difficulty range");
    Config settings;
    const auto names = settings.GetDifficultyPresets();
    ensure_equals(names.size(), size_t(4));
    ensure_equals(names[3], std::string("Nightmare"));
    settings.SetDifficultyPreset("Nightmare");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 1.0f);
    ensure_equals(settings["global_time_scale_mult"].toNumber<float>(), 1.0f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Nightmare"));
    ensure_equals(settings.GetClosestDifficulty(), std::string("Nightmare"));
    settings.SetDifficultyPreset("Expert");
    ensure("Expert clears the extra AI choice", !settings["campaign_mp_nightmare"].toBool());
    ensure_equals(settings.GetDifficultyPreset(), std::string("Expert"));
    ensure_equals(settings.GetClosestDifficulty(), std::string("Expert"));
}

template<> template<> void difficulty_test::test<2>() {
    set_test_name("existing lower presets and custom base settings cannot inherit Nightmare");
    Config settings;
    settings.SetDifficultyPreset("Nightmare");
    settings.SetDifficultyPreset("Casual");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 0.0f);
    ensure_equals(settings["global_time_scale_mult"].toNumber<float>(), 0.8f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Casual"));
    settings.SetDifficultyPreset("Hardcore");
    ensure_equals(settings["game_difficulty"].toNumber<float>(), 0.5f);
    ensure_equals(settings.GetDifficultyPreset(), std::string("Hardcore"));
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
    set_test_name("stat and reinforcement scaling is independent of AI preset selection");
    Config settings;
    settings.GetRef("campaign_mp_scaling") = false;
    settings.SetDifficultyPreset("Nightmare");
    ensure("choosing AI does not opt in to stat scaling", !settings["campaign_mp_scaling"].toBool());
    ensure_equals(settings.GetDifficultyPreset(), std::string("Nightmare"));
    settings.GetRef("campaign_mp_scaling") = true;
    settings.SetDifficultyPreset("Expert");
    ensure("choosing Expert retains stat scaling", settings["campaign_mp_scaling"].toBool());
}
}
