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
    ensure("expected progression pressure", std::fabs(state.Data().pressure - expected) < 1e-9);
}

template<> template<> void challenge_test::test<1>() {
    ChallengeScaling::Settings off;
    ChallengeScaling::State state;
    ensure(state.Advance(1800, off));
    ensure(state.Completion("level:one", off));
    ensure(state.Death("death:one", "encounter:one", off));
    ensure(state.Win("encounter:one", off));
    PressureNear(state, 0);
    ensure_equals(state.Data().active_seconds, 1800.0);
    const auto factors = state.Evaluate();
    ensure_equals(factors.vitality, 1.0f);
    ensure_equals(factors.damage, 1.0f);
    ensure_equals(factors.armor, 1.0f);
    ensure_equals(factors.ai, 0.0f);
}
template<> template<> void challenge_test::test<2>() {
    ChallengeScaling::Settings settings;
    settings.time_rate = settings.round_rate = settings.win_rate = settings.death_rate = ChallengeScaling::Standard;
    ChallengeScaling::State state;
    state.Advance(150, settings);
    PressureNear(state, 0.025);
    state.Advance(150, settings);
    PressureNear(state, 0.05);
    ensure(state.Completion("campaign:level1", settings));
    PressureNear(state, 0.13);
    ensure(state.Win("campaign:level1:goal4", settings));
    PressureNear(state, 0.17);
    ensure(state.Death("fatal:player1:one", "campaign:level1:goal5", settings));
    PressureNear(state, 0.22);
    ensure(!state.Completion("campaign:level1", settings));
    ensure(!state.Win("campaign:level1:goal4", settings));
    ensure(!state.Death("fatal:player1:one", "campaign:level1:goal5", settings));
    PressureNear(state, 0.22);
}
template<> template<> void challenge_test::test<3>() {
    ChallengeScaling::Settings settings;
    settings.win_rate = settings.round_rate = ChallengeScaling::Standard;
    settings.death_rate = ChallengeScaling::Brisk;
    settings.adaptive = true;
    ChallengeScaling::State state;
    state.Completion("level", settings);
    for (int attempt = 1; attempt <= 3; ++attempt)
        state.Death("failed-attempt:" + std::to_string(attempt), "same-logical-fight", settings);
    PressureNear(state, 0.08);
    ensure_equals(state.Data().encounters.at("same-logical-fight").deaths, uint32_t(3));
    ensure(state.Win("same-logical-fight", settings));
    PressureNear(state, 0.09);
    ensure("three deaths discount the pending win by 75 percent", std::fabs(state.Data().encounters.at("same-logical-fight").win_award - 0.01) < 1e-9);
    state.Death("late-notification", "same-logical-fight", settings);
    ensure(!state.Win("same-logical-fight", settings));
    PressureNear(state, 0.09);
    ensure("late deaths cannot reprice a credited victory", std::fabs(state.Data().encounters.at("same-logical-fight").win_award - 0.01) < 1e-9);
    for (int death = 0; death < 7; ++death) state.Death("hard-fight:" + std::to_string(death), "another-fight", settings);
    state.Win("another-fight", settings);
    ensure_equals("discounts cannot make a win a negative award", state.Data().encounters.at("another-fight").win_award, 0.0);
    PressureNear(state, 0.09);
}
template<> template<> void challenge_test::test<4>() {
    ChallengeScaling::Settings settings;
    ChallengeScaling::State state;
    ensure(state.Death("before-enabled", "", settings));
    PressureNear(state, 0);
    settings.death_rate = settings.win_rate = ChallengeScaling::Standard;
    ensure("enabling death pressure cannot reprice an earlier event", !state.Death("before-enabled", "", settings));
    for (int i = 0; i < 8; ++i) ensure(state.Death("human-down:" + std::to_string(i), "fight", settings));
    PressureNear(state, 0.4);
    ensure("player defeats strengthen enemy durability", state.Evaluate().vitality > 1.0f);
    ensure("a later KO-to-death notification cannot count the same defeat twice",
           !state.Death("human-down:0", "fight", settings));
    ensure_equals(state.Data().encounters.at("fight").deaths, uint32_t(8));
    PressureNear(state, 0.4);
    state.Win("later-win", settings);
    PressureNear(state, 0.44);
    settings.death_rate = ChallengeScaling::Gentle;
    state.Death("gentle-down", "", settings);
    PressureNear(state, 0.465);
    settings.death_rate = ChallengeScaling::Brisk;
    state.Death("brisk-down", "", settings);
    PressureNear(state, 0.54);
    state.Win("second-win", settings);
    PressureNear(state, 0.58);
    for (int i = 0; i < 30; ++i) state.Death("cap-down:" + std::to_string(i), "", settings);
    PressureNear(state, ChallengeScaling::kPressureCap);
}
template<> template<> void challenge_test::test<5>() {
    ChallengeScaling::Settings settings;
    settings.round_rate = settings.win_rate = ChallengeScaling::Standard;
    ChallengeScaling::State original;
    original.Completion("round", settings);
    original.Win("encounter", settings);
    ChallengeScaling::State restored;
    ensure(restored.Restore(original.Capture()));
    settings.round_rate = settings.win_rate = ChallengeScaling::Brisk;
    ensure(!restored.Completion("round", settings));
    ensure(!restored.Win("encounter", settings));
    PressureNear(restored, 0.12);
    restored.Advance(900, settings); // time progression remains off
    PressureNear(restored, 0.12);
    settings.time_rate = ChallengeScaling::Gentle;
    restored.Advance(300, settings);
    PressureNear(restored, 0.145);
    ensure_equals("turning time on cannot reprice earlier play", restored.Data().active_seconds, 1200.0);
    restored.Win("new-encounter", settings);
    PressureNear(restored, 0.205);
}
template<> template<> void challenge_test::test<6>() {
    ChallengeScaling::Settings settings;
    settings.time_rate = ChallengeScaling::Brisk;
    ChallengeScaling::State state;
    state.Advance(1e8, settings);
    PressureNear(state, ChallengeScaling::kPressureCap);
    const auto factors = state.Evaluate();
    ensure_equals(factors.vitality, 2.5f);
    ensure("damage remains gentle", std::fabs(factors.damage - 1.225f) < 1e-6f);
    ensure("armor remains gentle", std::fabs(factors.armor - 1.15f) < 1e-6f);
    ensure_equals("progression never changes AI", factors.ai, 0.0f);
    ensure(!state.Advance(-1, settings));
    ensure(!state.Advance(std::numeric_limits<double>::quiet_NaN(), settings));
    auto bad = state.Capture();
    bad.pressure = std::numeric_limits<double>::infinity();
    ensure(!state.Restore(bad));
    PressureNear(state, ChallengeScaling::kPressureCap);
    bad = state.Capture();
    bad.encounters["unearned"].win_award = 0.04;
    ensure(!state.Restore(bad));
    state.Clear();
    PressureNear(state, 0);
    ensure_equals(state.Data().active_seconds, 0.0);
    ensure(state.Data().encounters.empty());
}
}
