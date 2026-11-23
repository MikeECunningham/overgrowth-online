#include <Online/bot_follow_assignments.h>
#include <tut/tut.hpp>

namespace tut {
struct bot_follow_data {};
typedef test_group<bot_follow_data> bot_follow_group;
bot_follow_group bot_follow_tests("BotFollowAssignments");
typedef bot_follow_group::object follow_test;
using namespace BotFollowAssignments;

template<> template<> void follow_test::test<1>() {
    std::vector<Follower> bots;
    for (ID id = 10; id < 17; ++id) bots.push_back({id, kNone, {{1, 1}, {2, 100}, {3, 500}}});
    const auto result = Assign(bots);
    std::map<ID, int> loads;
    for (const auto& assignment : result) ++loads[assignment.second];
    ensure_equals(result.size(), size_t(7));
    for (ID id = 1; id <= 3; ++id) ensure("distance cannot pile every bot onto the closest human", loads[id] >= 2 && loads[id] <= 3);
}
template<> template<> void follow_test::test<2>() {
    std::vector<Follower> bots;
    for (ID id = 10; id < 15; ++id) bots.push_back({id, id < 13 ? ID(1) : ID(2), {{1, 1000}, {2, 1}}});
    const auto result = Assign(bots);
    for (const auto& bot : bots) ensure_equals("balanced valid buddies survive movement", result.at(bot.bot), bot.previous);
    for (auto& bot : bots) bot.candidates = {{2, 10}};
    const auto after_ko = Assign(bots);
    for (const auto& assignment : after_ko) ensure_equals("unavailable leaders are replaced", assignment.second, ID(2));
}
template<> template<> void follow_test::test<3>() {
    const auto result = Assign({{10, 1, {{1, 1}, {2, 10}}}, {11, 1, {{1, 1}, {2, 10}}},
                                {12, 1, {{1, 1}, {2, 10}}}, {13, 1, {{1, 1}, {2, 10}}}, {14, 1, {{1, 1}, {2, 10}}}});
    int moved = 0;
    for (const auto& assignment : result) if (assignment.second != 1) ++moved;
    ensure_equals("a new friendly human gets followers with minimal disruption", moved, 2);
    const auto hostile = Assign({{20, 3, {{1, 1}}}, {21, 2, {}}, {22, kNone, {{3, 1}}}});
    ensure_equals("old enemies cannot stay assigned", hostile.at(20), ID(1));
    ensure("no eligible human means no escort assignment", hostile.count(21) == 0);
    ensure_equals(hostile.at(22), ID(3));
}
template<> template<> void follow_test::test<4>() {
    // No direct move can improve these loads: a two-bot alternating path is
    // necessary to distribute an overlapping set of friendship associations.
    const auto result = Assign({{10, 1, {{1, 1}}}, {11, 1, {{1, 1}, {2, 1}}}, {12, 2, {{2, 1}, {3, 1}}}});
    std::map<ID, int> loads;
    for (const auto& assignment : result) ++loads[assignment.second];
    for (ID id = 1; id <= 3; ++id) ensure_equals("overlapping teams can rebalance through a valid chain", loads[id], 1);
}
}
