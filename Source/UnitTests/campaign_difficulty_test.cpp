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
    ensure_equals(CampaignDifficulty::ExtraEnemies(1, 12, false, CampaignDifficulty::Rat), 0);
    ensure_equals(CampaignDifficulty::ExtraEnemies(8, 0, false, CampaignDifficulty::Rat), 0);
}

template<> template<> void campaign_test::test<2>() {
    for (int humans = 2; humans <= 16; ++humans) {
        const auto regular = CampaignDifficulty::ForPlayers(humans, false, CampaignDifficulty::CatOrDog, true, false);
        const auto boss = CampaignDifficulty::ForPlayers(humans, true, CampaignDifficulty::CatOrDog, true, false);
        ensure("boss durability is the principal scaling lever", boss.vitality > regular.vitality);
        ensure("boss damage remains a gentler increase than effective health", boss.vitality > boss.damage);
        ensure_equals("the normal difficulties never alter authored AI", regular.ai, 0.0f);
    }
    const auto nightmare = CampaignDifficulty::ForPlayers(4, true, CampaignDifficulty::CatOrDog, false, true);
    ensure_equals("AI scaling is independent from the stat checkbox", nightmare.vitality, 1.0f);
    ensure("Nightmare improves AI", nightmare.ai > 0.0f);
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
            ensure("armor and AI have hard caps", current.armor <= 1.9f && current.ai <= 0.35f);
            const int adds = CampaignDifficulty::ExtraEnemies(humans, 30, false, rank);
            ensure("regular encounter additions have a hard cap", adds >= previous_adds && adds <= CampaignDifficulty::kRegularExtraCap);
            ensure("boss bodyguards have a hard cap", CampaignDifficulty::ExtraEnemies(humans, 1, true, rank) <= CampaignDifficulty::kBossExtraCap);
            previous = current;
            previous_adds = adds;
        }
    }
    ensure("large parties have diminishing marginal scaling",
        CampaignDifficulty::ExtraPlayers(6) - CampaignDifficulty::ExtraPlayers(5) <
        CampaignDifficulty::ExtraPlayers(4) - CampaignDifficulty::ExtraPlayers(3));
}

template<> template<> void campaign_test::test<4>() {
    ensure("rats gain numbers before wolves", CampaignDifficulty::ExtraEnemies(2, 4, false, CampaignDifficulty::Rat) >
        CampaignDifficulty::ExtraEnemies(2, 4, false, CampaignDifficulty::Wolf));
    ensure_equals("two humans do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(2, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("four humans do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(4, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("large parties may face one same-species wolf bodyguard", CampaignDifficulty::ExtraEnemies(8, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("ordinary bosses may gain one guard at four humans", CampaignDifficulty::ExtraEnemies(4, 1, true, CampaignDifficulty::CatOrDog), 1);
}
}
