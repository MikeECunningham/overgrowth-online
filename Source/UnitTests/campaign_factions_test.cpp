#include <Online/campaign_factions.h>
#include <tut/tut.hpp>

namespace tut {
struct campaign_factions_data {};
typedef test_group<campaign_factions_data> campaign_factions_group;
campaign_factions_group campaign_factions_tests("CampaignFactions");
typedef campaign_factions_group::object faction_test;

template<> template<> void faction_test::test<1>() {
    using namespace CampaignFactions;
    ensure_equals(Canonical(" b, c, b "), std::string("b,c"));
    const auto profiles = EnemyProfiles("a", {"a", "a,b", "b", "c,b", "c", "b,c", ""});
    ensure_equals(profiles.size(), size_t(3));
    ensure_equals(profiles[0], std::string("b"));
    ensure_equals(profiles[1], std::string("b,c"));
    ensure_equals(profiles[2], std::string("c"));
    ensure("a bridging faction does not merge distinct enemies", !Friendly(profiles[0], profiles[2]));
    ensure("the bridge retains each friendship", Friendly(profiles[0], profiles[1]) && Friendly(profiles[1], profiles[2]));
}
template<> template<> void faction_test::test<2>() {
    using namespace CampaignFactions;
    const std::vector<std::string> profiles = {"b", "b,c", "c"};
    const auto assignment = Assign(7, {1, 2, 5, 7, 9, 10}, "a", profiles, "unique_");
    ensure("MC need not be the lowest lobby team", Friendly(assignment.at(7), "a"));
    ensure("opponents map in ascending team order", Friendly(assignment.at(1), "b") && !Friendly(assignment.at(1), "c"));
    ensure("the next team gets the complete second profile", Friendly(assignment.at(2), "b") && Friendly(assignment.at(2), "c"));
    ensure("third opposing team gets the final profile", Friendly(assignment.at(5), "c") && !Friendly(assignment.at(5), "b"));
    ensure("wildcard teams are hostile to each other", !Friendly(assignment.at(9), assignment.at(10)));
    for (int team : {1, 2, 5, 7}) ensure("wildcards remain hostile to mapped teams", !Friendly(assignment.at(9), assignment.at(team)));
    ensure("wildcards remain hostile to every NPC profile", !Friendly(assignment.at(9), "a,b,c"));
    ensure("members of one wildcard team still cooperate", Friendly(assignment.at(9), assignment.at(9)));
}
template<> template<> void faction_test::test<3>() {
    using namespace CampaignFactions;
    const auto prefix = UnusedPrefix({"a", "a,__og_campaign_lobby_2", "b,__og_campaign_lobby__3"});
    ensure_equals(prefix, std::string("__og_campaign_lobby___"));
    const auto assignment = Assign(3, {1, 3, 5}, "", {}, prefix);
    ensure("an untagged MC party still cooperates internally", Friendly(assignment.at(3), assignment.at(3)));
    ensure("unaffiliated teams remain distinct", !Friendly(assignment.at(1), assignment.at(3)) && !Friendly(assignment.at(1), assignment.at(5)));
    ensure("synthetic tags cannot accidentally join an authored faction", !Friendly(assignment.at(1), "__og_campaign_lobby_1,__og_campaign_lobby__1"));
}
template<> template<> void faction_test::test<4>() {
    using namespace CampaignFactions;
    MainAnchor anchor;
    std::map<uint32_t, int> humans = {{100, 7}, {1, 2}, {256, 7}};
    anchor.Refresh(humans, 100);
    ensure_equals(anchor.Resolve(humans, 100), 7);
    humans.erase(100);
    ensure_equals("lowest opposing player cannot inherit MC affiliation", anchor.Resolve(humans, 1), 7);
    anchor.Refresh(humans, 1);
    ensure_equals("an opposing fallback cannot become the stored anchor", anchor.player, uint32_t(100));
    anchor.Refresh(humans, 256);
    ensure_equals("a surviving MC teammate can become the story anchor", anchor.player, uint32_t(256));
    humans[256] = 9;
    ensure_equals("an explicit team edit follows the actual surviving MC", anchor.Resolve(humans, 256), 9);
    anchor.Refresh(humans, 256);
    humans.erase(256);
    ensure_equals("the last MC team remains reserved after its final human leaves", anchor.Resolve(humans, 1), 9);
}
}
