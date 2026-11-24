#include <Online/campaign_difficulty.h>
#include <tut/tut.hpp>

namespace tut {
struct campaign_difficulty_data {};
typedef test_group<campaign_difficulty_data> campaign_difficulty_group;
campaign_difficulty_group campaign_difficulty_tests("CampaignDifficulty");
typedef campaign_difficulty_group::object campaign_test;

template<> template<> void campaign_test::test<1>() {
    for (int rank = CampaignDifficulty::Rat; rank <= CampaignDifficulty::Wolf; ++rank) {
        const auto solo = CampaignDifficulty::ForPlayers(1, true, rank, true, true);
        ensure_equals(solo.vitality, 1.0f);
        ensure_equals(solo.damage, 1.0f);
        ensure_equals(solo.armor, 1.0f);
        ensure_equals(solo.ai, 0.0f);
        const auto disabled = CampaignDifficulty::ForPlayers(8, false, rank, false, false);
        ensure_equals(disabled.vitality, 1.0f);
        ensure_equals(disabled.damage, 1.0f);
        ensure_equals(disabled.armor, 1.0f);
        ensure_equals(disabled.ai, 0.0f);
    }
    ensure_equals(CampaignDifficulty::ExtraEnemiesForParty(1, 0, 12, false, CampaignDifficulty::Rat), 0);
    ensure_equals(CampaignDifficulty::ExtraEnemiesForParty(8, 0, 0, false, CampaignDifficulty::Rat), 0);
}

template<> template<> void campaign_test::test<2>() {
    for (int humans = 2; humans <= 16; ++humans) {
        const auto regular = CampaignDifficulty::ForPlayers(humans, false, CampaignDifficulty::CatOrDog, true, false);
        const auto boss = CampaignDifficulty::ForPlayers(humans, true, CampaignDifficulty::CatOrDog, true, false);
        ensure("boss durability is the principal scaling lever", boss.vitality > regular.vitality);
        ensure("boss damage remains a gentler increase than effective health", boss.vitality > boss.damage);
        ensure_equals("the normal difficulties never alter authored AI", regular.ai, 0.0f);
    }
    const auto nightmare = CampaignDifficulty::ForPlayers(4, true, CampaignDifficulty::CatOrDog, false, true, 5);
    ensure_equals("AI scaling is independent from the stat checkbox", nightmare.vitality, 1.0f);
    ensure("Nightmare permits a small selected-unit variation", nightmare.ai > 0.0f && nightmare.ai <= 0.01f);
}

template<> template<> void campaign_test::test<3>() {
    for (int rank = CampaignDifficulty::Rat; rank <= CampaignDifficulty::Wolf; ++rank) {
        auto previous = CampaignDifficulty::ForPlayers(1, true, rank, true, true);
        int previous_adds = 0;
        for (int humans = 2; humans <= 128; ++humans) {
            const auto current = CampaignDifficulty::ForPlayers(humans, true, rank, true, true);
            ensure("durability never falls with another human", current.vitality >= previous.vitality);
            ensure("damage never falls with another human", current.damage >= previous.damage);
            ensure("armor never falls with another human", current.armor >= previous.armor);
            ensure("AI never falls with another human", current.ai >= previous.ai);
            ensure("boss durability and damage have hard caps", current.vitality <= 6.0f && current.damage <= 1.9f);
            ensure("armor and AI have hard caps", current.armor <= 1.9f && current.ai <= 0.03f);
            const int adds = CampaignDifficulty::ExtraEnemiesForParty(humans, 0, 30, false, rank);
            ensure("regular encounter additions have a hard cap", adds >= previous_adds && adds <= CampaignDifficulty::kRegularExtraCap);
            ensure("boss bodyguards have a hard cap", CampaignDifficulty::ExtraEnemiesForParty(humans, 0, 1, true, rank) <= CampaignDifficulty::kBossExtraCap);
            previous = current;
            previous_adds = adds;
        }
    }
    ensure("large parties have diminishing marginal scaling",
        CampaignDifficulty::ExtraPlayers(6) - CampaignDifficulty::ExtraPlayers(5) <
        CampaignDifficulty::ExtraPlayers(4) - CampaignDifficulty::ExtraPlayers(3));
}

template<> template<> void campaign_test::test<4>() {
    ensure("weak regulars gain more numbers than strong regulars", CampaignDifficulty::ExtraEnemiesForParty(4, 0, 4, false, CampaignDifficulty::Rat) >
        CampaignDifficulty::ExtraEnemiesForParty(4, 0, 4, false, CampaignDifficulty::Wolf));
    ensure_equals("two humans do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemiesForParty(2, 0, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("three humans may face one same-species wolf bodyguard", CampaignDifficulty::ExtraEnemiesForParty(3, 0, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("large wolf parties still add at most one wolf", CampaignDifficulty::ExtraEnemiesForParty(64, 0, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("large dog bosses also retain the one-guard cap", CampaignDifficulty::ExtraEnemiesForParty(64, 0, 1, true, CampaignDifficulty::BigDog), 1);
    ensure_equals("ordinary bosses gain one guard before they gain two", CampaignDifficulty::ExtraEnemiesForParty(3, 0, 1, true, CampaignDifficulty::CatOrDog), 1);
    ensure_equals("ordinary bosses may gain two guards for a larger human party", CampaignDifficulty::ExtraEnemiesForParty(8, 0, 1, true, CampaignDifficulty::CatOrDog), 2);
    ensure_equals("ordinary boss groups never exceed two guards", CampaignDifficulty::ExtraEnemiesForParty(64, 0, 1, true, CampaignDifficulty::CatOrDog), 2);
}

template<> template<> void campaign_test::test<5>() {
    const auto rabbit = CampaignDifficulty::ForPlayers(4, false, CampaignDifficulty::Rabbit, true, false);
    const auto wolf = CampaignDifficulty::ForPlayers(4, true, CampaignDifficulty::Wolf, true, false);
    ensure("the unweighted rabbit stat helper preserves its curve", std::fabs(rabbit.vitality - 1.54825f) < 0.00001f);
    ensure("the unweighted wolf stat helper preserves its curve", std::fabs(wolf.vitality - 3.4735f) < 0.00001f);
    ensure_equals("three extra humans add three rabbit reinforcements",
                  CampaignDifficulty::ExtraEnemiesForParty(4, 0, 4, false, CampaignDifficulty::Rabbit), 3);
    ensure_equals("one human helper adds one enemy even to a large crowd", CampaignDifficulty::ExtraEnemiesForParty(2, 0, 30, false, CampaignDifficulty::Rat), 1);
    ensure_equals("two human helpers add two enemies", CampaignDifficulty::ExtraEnemiesForParty(3, 0, 30, false, CampaignDifficulty::Rabbit), 2);
    ensure_equals("a large original crowd cannot multiply the addition budget", CampaignDifficulty::ExtraEnemiesForParty(4, 0, 30, false, CampaignDifficulty::Rat), 3);
    ensure_equals("larger human parties show diminishing count growth", CampaignDifficulty::ExtraEnemiesForParty(7, 0, 4, false, CampaignDifficulty::Rabbit), 4);
    ensure_equals("strong-species groups retain a smaller count increase", CampaignDifficulty::ExtraEnemiesForParty(7, 0, 4, false, CampaignDifficulty::CatOrDog), 3);
    ensure_equals("concurrent additions remain bounded without exhausting the chapter", CampaignDifficulty::kConcurrentExtraCap, 8);
    ensure_equals("live hostile cap is unchanged", CampaignDifficulty::kActorCap, 32);
}

template<> template<> void campaign_test::test<6>() {
    ensure_equals("unspecified units never receive AI boosts", CampaignDifficulty::ForPlayers(32, true, CampaignDifficulty::Wolf, true, true).ai, 0.0f);
    ensure_equals("small parties have no extra AI pressure", CampaignDifficulty::ForPlayers(3, true, CampaignDifficulty::Wolf, true, true, 5).ai, 0.0f);
    ensure_equals(CampaignDifficulty::NightmareAIAmount(4), 0.01f);
    ensure_equals(CampaignDifficulty::NightmareAIAmount(8), 0.02f);
    ensure_equals(CampaignDifficulty::NightmareAIAmount(16), 0.03f);
    int selected = 0;
    int traits[4] = {0, 0, 0, 0};
    for (uint32_t id = 0; id < 256; ++id) {
        const auto trait = CampaignDifficulty::TweakForUnit(id);
        const auto chosen = CampaignDifficulty::ForPlayers(4, false, CampaignDifficulty::Rabbit, false, true, id);
        const auto ordinary = CampaignDifficulty::ForPlayers(32, false, CampaignDifficulty::Rabbit, false, false, id);
        ensure_equals("normal difficulties preserve every unit's authored AI", ordinary.ai, 0.0f);
        if (trait != CampaignDifficulty::NoTweak) {
            ++selected; ++traits[trait];
            ensure_equals("a selected unit gets one modest tier", chosen.ai, 0.01f);
        } else ensure_equals("most units receive no AI modifier", chosen.ai, 0.0f);
        ensure_equals("party size cannot change a unit's trait", CampaignDifficulty::TweakForUnit(id), trait);
    }
    ensure_equals("only one in eight identities is selected", selected, 32);
    for (int trait = 1; trait <= 3; ++trait) ensure("offensive traits are distributed", traits[trait] > 0);
}

template<> template<> void campaign_test::test<7>() {
    const auto humans = CampaignDifficulty::ForParty(4, 0, false, CampaignDifficulty::Rabbit, true, false);
    const auto bots = CampaignDifficulty::ForParty(1, 3, false, CampaignDifficulty::Rabbit, true, false);
    ensure("four humans receive more scaling than the previous unweighted curve", std::fabs(humans.vitality - 1.7484426f) < 0.00001f);
    ensure("three friendly bots provide only modest additional scaling", std::fabs(bots.vitality - 1.1918875f) < 0.00001f);
    ensure("equal headcounts distinguish humans from bots", humans.vitality > bots.vitality && humans.damage > bots.damage && humans.armor > bots.armor);
    ensure("one human ally weighs four friendly bots",
           std::fabs(CampaignDifficulty::ForParty(2, 0, true, CampaignDifficulty::Wolf, true, false).vitality -
                     CampaignDifficulty::ForParty(1, 4, true, CampaignDifficulty::Wolf, true, false).vitality) < 0.00001f);
    ensure_equals("a solo human remains unscaled", CampaignDifficulty::ForParty(1, 0, true, CampaignDifficulty::Wolf, true, true, 5).vitality, 1.0f);
    ensure_equals("temporary empty rosters retain the neutral baseline", CampaignDifficulty::EffectivePartySize(0, 0), 1.0f);
    ensure_equals("invalid negative counts cannot reduce authored difficulty", CampaignDifficulty::EffectivePartySize(-50, -50), 1.0f);
    ensure_equals("extreme counts remain bounded without overflow", CampaignDifficulty::EffectivePartySize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()), 64.0f);
    const auto off = CampaignDifficulty::ForParty(32, 32, true, CampaignDifficulty::Wolf, false, false);
    ensure_equals(off.vitality, 1.0f);
    ensure_equals(off.damage, 1.0f);
    ensure_equals(off.armor, 1.0f);
}

template<> template<> void campaign_test::test<8>() {
    ensure_equals("a solo human receives no additions", CampaignDifficulty::ExtraEnemiesForParty(1, 0, 4, false, CampaignDifficulty::Rabbit), 0);
    ensure_equals("the first human ally adds one ordinary enemy", CampaignDifficulty::ExtraEnemiesForParty(2, 0, 4, false, CampaignDifficulty::Rabbit), 1);
    ensure_equals("the first friendly bot also adds one ordinary enemy", CampaignDifficulty::ExtraEnemiesForParty(1, 1, 4, false, CampaignDifficulty::Rabbit), 1);
    ensure_equals("the second human ally adds a second enemy", CampaignDifficulty::ExtraEnemiesForParty(3, 0, 4, false, CampaignDifficulty::Rabbit), 2);
    ensure_equals("two bots contribute one human's count weight", CampaignDifficulty::ExtraEnemiesForParty(1, 2, 4, false, CampaignDifficulty::Rabbit), 1);
    ensure_equals("three bots add two weak enemies", CampaignDifficulty::ExtraEnemiesForParty(1, 3, 4, false, CampaignDifficulty::Rabbit), 2);
    ensure_equals("seven bots visibly add three weak enemies", CampaignDifficulty::ExtraEnemiesForParty(1, 7, 4, false, CampaignDifficulty::Rabbit), 3);
    ensure_equals("seven bots add two stronger enemies", CampaignDifficulty::ExtraEnemiesForParty(1, 7, 4, false, CampaignDifficulty::CatOrDog), 2);
    ensure_equals("the first bot also counts against stronger regulars", CampaignDifficulty::ExtraEnemiesForParty(1, 1, 4, false, CampaignDifficulty::Wolf), 1);
    ensure_equals("original crowd size cannot multiply the count budget", CampaignDifficulty::ExtraEnemiesForParty(1, 7, 30, false, CampaignDifficulty::Rabbit), 3);
    ensure_equals("four humans gain three weak enemies", CampaignDifficulty::ExtraEnemiesForParty(4, 0, 4, false, CampaignDifficulty::Rabbit), 3);
    ensure_equals("the count curve falls off instead of matching every ally", CampaignDifficulty::ExtraEnemiesForParty(8, 0, 4, false, CampaignDifficulty::Rabbit), 4);
    ensure_equals("ordinary bosses do not gain a guard for just one ally", CampaignDifficulty::ExtraEnemiesForParty(2, 0, 1, true, CampaignDifficulty::CatOrDog), 0);
    ensure_equals("three bots can add one ordinary boss guard", CampaignDifficulty::ExtraEnemiesForParty(1, 3, 1, true, CampaignDifficulty::CatOrDog), 1);
    ensure_equals("large-dog bosses wait beyond three bots", CampaignDifficulty::ExtraEnemiesForParty(1, 3, 1, true, CampaignDifficulty::BigDog), 0);
    ensure_equals("large-dog bosses gain a guard at four bots", CampaignDifficulty::ExtraEnemiesForParty(1, 4, 1, true, CampaignDifficulty::BigDog), 1);
    ensure_equals("wolf bosses wait beyond three bots", CampaignDifficulty::ExtraEnemiesForParty(1, 3, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("wolf round-half boundary counts four bots", CampaignDifficulty::ExtraEnemiesForParty(1, 4, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("the same wolf boundary holds for mixed humans and bots", CampaignDifficulty::ExtraEnemiesForParty(2, 2, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("a human ally contributes exactly two bots to the count curve", CampaignDifficulty::ExtraEnemiesForParty(3, 0, 4, false, CampaignDifficulty::Rabbit), CampaignDifficulty::ExtraEnemiesForParty(2, 2, 4, false, CampaignDifficulty::Rabbit));
    ensure_equals("missing original actors cannot produce reinforcements", CampaignDifficulty::ExtraEnemiesForParty(64, 64, 0, false, CampaignDifficulty::Rabbit), 0);
    ensure_equals("invalid negative parties cannot produce reinforcements", CampaignDifficulty::ExtraEnemiesForParty(std::numeric_limits<int>::min(), std::numeric_limits<int>::min(), 4, false, CampaignDifficulty::Rabbit), 0);
    ensure_equals("extreme counts cannot overflow or exceed the regular cap", CampaignDifficulty::ExtraEnemiesForParty(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 4, false, CampaignDifficulty::Rabbit), 6);
    for (int rank = CampaignDifficulty::Rat; rank <= CampaignDifficulty::Wolf; ++rank) {
        for (int humans = 1; humans <= 128; ++humans) {
            int previous = 0, previous_boss = 0;
            for (int bots = 0; bots <= 128; ++bots) {
                const int additions = CampaignDifficulty::ExtraEnemiesForParty(humans, bots, 30, false, rank);
                const int guards = CampaignDifficulty::ExtraEnemiesForParty(humans, bots, 1, true, rank);
                ensure("additional bots cannot decrease the bounded reinforcement budget", additions >= previous && additions <= CampaignDifficulty::kRegularExtraCap);
                ensure("another human cannot reduce the reinforcement budget", additions >= CampaignDifficulty::ExtraEnemiesForParty(humans - 1, bots, 30, false, rank));
                ensure("boss guards remain monotonic and capped", guards >= previous_boss && guards <= (rank >= CampaignDifficulty::BigDog ? 1 : CampaignDifficulty::kBossExtraCap));
                previous = additions;
                previous_boss = guards;
            }
        }
    }
}

template<> template<> void campaign_test::test<9>() {
    ensure_equals("a large bot party does not unlock Nightmare AI", CampaignDifficulty::ForParty(1, 63, true, CampaignDifficulty::Wolf, true, true, 5).ai, 0.0f);
    ensure_equals("bots cannot cross the first human-only AI tier", CampaignDifficulty::ForParty(3, 63, true, CampaignDifficulty::Wolf, true, true, 5).ai, 0.0f);
    ensure_equals("four humans unlock the rare one-percent tier", CampaignDifficulty::ForParty(4, 32, true, CampaignDifficulty::Wolf, false, true, 5).ai, 0.01f);
    ensure_equals("eight humans unlock the rare two-percent tier", CampaignDifficulty::ForParty(8, 0, true, CampaignDifficulty::Wolf, false, true, 5).ai, 0.02f);
    ensure_equals("sixteen humans unlock the rare three-percent tier", CampaignDifficulty::ForParty(16, 0, true, CampaignDifficulty::Wolf, false, true, 5).ai, 0.03f);
    for (int rank = CampaignDifficulty::Rat; rank <= CampaignDifficulty::Wolf; ++rank) {
        auto previous = CampaignDifficulty::ForParty(1, 0, true, rank, true, true, 5);
        for (int humans = 1; humans <= 128; ++humans) {
            const auto current = CampaignDifficulty::ForParty(humans, humans, true, rank, true, true, 5);
            ensure("mixed party durability is monotonic and capped", current.vitality >= previous.vitality && current.vitality <= 6.0f);
            ensure("mixed party damage is monotonic and capped", current.damage >= previous.damage && current.damage <= 1.9f);
            ensure("mixed party armor is monotonic and capped", current.armor >= previous.armor && current.armor <= 1.9f);
            ensure("AI retains its human-only cap", current.ai <= 0.03f);
            previous = current;
        }
    }
}
}
