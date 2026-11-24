#include <Online/challenge_scaling.h>
#include <tut/tut.hpp>
#include <cmath>
#include <limits>

namespace tut {
struct challenge_scaling_data {};
typedef test_group<challenge_scaling_data> challenge_group;
challenge_group challenge_tests("ChallengeScaling");
typedef challenge_group::object challenge_test;

static void PressureNear(const ChallengeScaling::State& state, double expected) {
    ensure("expected general progression pressure", std::fabs(state.Data().pressure - expected) < 1e-9);
}
static void Near(double actual, double expected) { ensure("expected pressure or award", std::fabs(actual - expected) < 1e-9); }

template<> template<> void challenge_test::test<1>() {
    ChallengeScaling::Settings off;
    ChallengeScaling::State state;
    ensure(state.Advance(1800, off));
    ensure(state.Completion("level:one", off));
    ensure(state.Wipe("wipe:one", "encounter:one", off));
    ensure(state.Win("encounter:one", off));
    PressureNear(state, 0);
    ensure_equals(state.Data().active_seconds, 1800.0);
    const auto factors = state.Evaluate();
    ensure_equals(factors.vitality, 1.0f);
    ensure_equals(factors.damage, 1.0f);
    ensure_equals("armor is physical gear", factors.armor, 1.0f);
    ensure_equals(factors.ai, 0.0f);
}
template<> template<> void challenge_test::test<2>() {
    ChallengeScaling::Settings settings;
    settings.time_rate = settings.round_rate = ChallengeScaling::Standard;
    settings.performance_mode = ChallengeScaling::OnWin;
    ChallengeScaling::State state;
    state.Advance(150, settings); PressureNear(state, 0.025);
    state.Advance(150, settings); PressureNear(state, 0.05);
    ensure(state.Completion("campaign:level1", settings)); PressureNear(state, 0.13);
    ensure(state.Win("campaign:level1:goal4", settings)); PressureNear(state, 0.17);
    ensure(state.Wipe("wipe:1", "campaign:level1:goal5", settings)); PressureNear(state, 0.17);
    ensure(!state.Completion("campaign:level1", settings));
    ensure(!state.Win("campaign:level1:goal4", settings));
    ensure(!state.Wipe("wipe:1", "campaign:level1:goal5", settings));
    PressureNear(state, 0.17);
}
template<> template<> void challenge_test::test<3>() {
    ChallengeScaling::Settings settings;
    settings.performance_mode = ChallengeScaling::OnLoss;
    ChallengeScaling::State state;
    ensure(state.Wipe("wipe:one", "fight", settings)); PressureNear(state, 0.05);
    ensure(!state.Wipe("wipe:one", "fight", settings)); PressureNear(state, 0.05);
    ensure(state.Win("fight", settings)); PressureNear(state, 0.05);
    settings.performance_rate = ChallengeScaling::Gentle;
    state.Wipe("wipe:two", "fight2", settings); PressureNear(state, 0.075);
    settings.performance_rate = ChallengeScaling::Brisk;
    state.Wipe("wipe:three", "fight2", settings); PressureNear(state, 0.15);
    for (int i = 0; i < 30; ++i) state.Wipe("cap:" + std::to_string(i), "fight2", settings);
    PressureNear(state, ChallengeScaling::kPressureCap);
}
template<> template<> void challenge_test::test<4>() {
    ChallengeScaling::Settings settings;
    settings.performance_mode = ChallengeScaling::Forced50;
    ChallengeScaling::State state;
    state.Wipe("attempt:1", "same-fight", settings);
    PressureNear(state, 0);
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "same-fight"), -0.025);
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "next-fight"), 0);
    const auto preview = ChallengeScaling::PreviewWin(state.Data(), settings, "same-fight");
    Near(preview.pressure, -0.025); Near(preview.general_pressure, 0); Near(preview.encounter_relief, 0.025);
    Near(preview.award, 0.08); Near(preview.next_pressure, 0.08);
    Near(preview.wipe_award, -0.025);
    ensure_equals(preview.encounter_wipes, uint32_t(1));
    ensure(state.Win("same-fight", settings)); PressureNear(state, 0.08);
    Near(state.Data().encounters.at("same-fight").relief, 0);
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "next-fight"), 0.08);
    state.Wipe("late-defeat", "same-fight", settings);
    ensure(!state.Win("same-fight", settings)); PressureNear(state, 0.08);
    Near(state.Data().encounters.at("same-fight").relief, 0);
}
template<> template<> void challenge_test::test<5>() {
    ChallengeScaling::Settings settings;
    settings.performance_mode = ChallengeScaling::Forced50;
    settings.round_rate = ChallengeScaling::Standard;
    ChallengeScaling::State state;
    state.Completion("level", settings);
    for (int i = 0; i < 20; ++i) state.Wipe("attempt:" + std::to_string(i), "fight", settings);
    PressureNear(state, 0.08);
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "fight"), -0.17);
    Near(state.Data().encounters.at("fight").relief, ChallengeScaling::kRetryReliefCap);
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "unrelated-fight"), 0.08);
    // Switching modes cannot turn old local relief into permanent progression.
    settings.performance_mode = ChallengeScaling::OnWin;
    Near(ChallengeScaling::EffectivePressure(state.Data(), settings, "fight"), 0.08);
    state.Win("fight", settings); PressureNear(state, 0.12);
    Near(state.Data().encounters.at("fight").relief, 0);
}
template<> template<> void challenge_test::test<6>() {
    ChallengeScaling::Settings settings;
    settings.performance_mode = ChallengeScaling::Forced50;
    settings.round_rate = ChallengeScaling::Standard;
    ChallengeScaling::State original;
    original.Completion("round", settings);
    original.Wipe("attempt:1", "encounter", settings);
    original.Wipe("attempt:2", "encounter", settings);
    ChallengeScaling::State restored;
    ensure(restored.Restore(original.Capture()));
    Near(ChallengeScaling::EffectivePressure(restored.Data(), settings, "encounter"), 0.03);
    ensure(!restored.Wipe("attempt:2", "encounter", settings));
    ensure(!restored.Completion("round", settings));
    settings.performance_rate = ChallengeScaling::Brisk;
    ensure(restored.Win("encounter", settings)); PressureNear(restored, 0.20);
    const auto credited = restored.Capture();
    ensure(original.Restore(credited));
    settings.performance_rate = ChallengeScaling::Gentle;
    ensure(!original.Win("encounter", settings)); PressureNear(original, 0.20);
}
template<> template<> void challenge_test::test<7>() {
    ChallengeScaling::WipeTracker tracker;
    ensure(!tracker.Observe(2, 2, 1000));
    ensure(!tracker.Observe(2, 1, 1100));
    ensure("only a total human-party defeat counts", tracker.Observe(2, 0, 1200));
    ensure(!tracker.Observe(2, 0, 1500));
    ensure(!tracker.Observe(2, 1, 1800));
    ensure(!tracker.Observe(2, 0, 1900));
    auto migrated = tracker;
    migrated.all_awake_since = 0;
    ensure("migration while dead never duplicates the wipe", !migrated.Observe(2, 0, 2200));
    ensure(!migrated.Observe(2, 2, 2300));
    ensure(!migrated.Observe(2, 0, 2400)); // transient all-up is not a fresh attempt
    ensure(!migrated.Observe(2, 2, 2500));
    ensure(!migrated.Observe(2, 2, 2900));
    ensure(migrated.Observe(2, 0, 3000));
    ensure(!migrated.Observe(0, 0, 3100));
    ChallengeScaling::WipeTracker sole_human;
    ensure("surviving bots are absent from human counts", sole_human.Observe(1, 0, 100));
}
template<> template<> void challenge_test::test<8>() {
    const auto relieved = ChallengeScaling::Evaluate(-0.25);
    ensure_equals("retry relief really eases baseline enemies", relieved.vitality, 0.75f);
    ensure("damage relief is gentle", std::fabs(relieved.damage - 0.9625f) < 1e-6f);
    ensure_equals(relieved.armor, 1.0f);
    const auto capped = ChallengeScaling::Evaluate(1e8);
    ensure_equals(capped.vitality, 2.5f);
    ensure(std::fabs(capped.damage - 1.225f) < 1e-6f);
    ensure_equals("no invisible armor multiplier", capped.armor, 1.0f);
    ensure_equals(capped.ai, 0.0f);
    ensure_equals(ChallengeScaling::Evaluate(-1e8).vitality, 0.75f);
    ensure_equals(ChallengeScaling::Evaluate(std::numeric_limits<double>::quiet_NaN()).vitality, 1.0f);
}
template<> template<> void challenge_test::test<9>() {
    ChallengeScaling::Settings settings;
    settings.time_rate = ChallengeScaling::Brisk;
    ChallengeScaling::State state;
    state.Advance(1e8, settings); PressureNear(state, ChallengeScaling::kPressureCap);
    ensure(!state.Advance(-1, settings));
    ensure(!state.Advance(std::numeric_limits<double>::quiet_NaN(), settings));
    auto bad = state.Capture(); bad.pressure = std::numeric_limits<double>::infinity();
    ensure(!state.Restore(bad));
    bad = state.Capture(); bad.encounters["unearned"].win_award = 0.04;
    ensure(!state.Restore(bad));
    bad = state.Capture(); bad.encounters["unearned"].relief = 0.1;
    ensure(!state.Restore(bad));
    bad = state.Capture(); bad.wipes.insert("wipe"); bad.encounters["fight"].wipes = 1; bad.encounters["fight"].relief = 0.25;
    ensure(!state.Restore(bad));
    state.Clear(); PressureNear(state, 0);
    ensure(state.Data().encounters.empty());
}
template<> template<> void challenge_test::test<10>() {
    ChallengeScaling::Settings settings;
    ChallengeScaling::State state;
    state.Win("off-win", settings);
    state.Wipe("off-loss", "fight", settings);
    settings.performance_mode = ChallengeScaling::OnWin;
    settings.performance_rate = ChallengeScaling::Gentle;
    ensure(!state.Win("off-win", settings));
    state.Win("gentle-win", settings); PressureNear(state, 0.02);
    settings.performance_rate = ChallengeScaling::Brisk;
    state.Win("brisk-win", settings); PressureNear(state, 0.08);
    settings.performance_mode = ChallengeScaling::OnLoss;
    ensure(!state.Wipe("off-loss", "fight", settings)); PressureNear(state, 0.08);
    settings.performance_rate = ChallengeScaling::Off;
    state.Wipe("rate-off", "fight", settings); PressureNear(state, 0.08);
    ensure_equals(state.Data().wipes.size(), size_t(2));
}
template<> template<> void challenge_test::test<11>() {
    ChallengeScaling::Settings settings;
    settings.time_rate = settings.round_rate = settings.performance_rate = ChallengeScaling::Nightmare;
    settings.performance_mode = ChallengeScaling::Forced50;
    ChallengeScaling::State state;
    state.Advance(300,settings); PressureNear(state,.10);
    state.Completion("level",settings); PressureNear(state,.26);
    state.Wipe("wipe","fight",settings);
    Near(ChallengeScaling::EffectivePressure(state.Data(),settings,"fight"),.21);
    state.Win("fight",settings); PressureNear(state,.42);
    ChallengeScaling::State migrated;
    ensure("Nightmare awards survive migration validation",migrated.Restore(state.Capture()));
    auto invalid = state.Capture(); invalid.encounters["fight"].win_award = .1601;
    ensure("awards above the strongest supported rate are rejected",!migrated.Restore(invalid));
}
}
