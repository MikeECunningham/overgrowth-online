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
    ensure("rats gain numbers before wolves", CampaignDifficulty::ExtraEnemies(4, 4, false, CampaignDifficulty::Rat) >
        CampaignDifficulty::ExtraEnemies(4, 4, false, CampaignDifficulty::Wolf));
    ensure_equals("two humans do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(2, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("four humans do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(4, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("six participants do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(6, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("eight participants still do not duplicate a wolf boss", CampaignDifficulty::ExtraEnemies(8, 1, true, CampaignDifficulty::Wolf), 0);
    ensure_equals("twelve participants may face one same-species wolf bodyguard", CampaignDifficulty::ExtraEnemies(12, 1, true, CampaignDifficulty::Wolf), 1);
    ensure_equals("ordinary bosses do not gain a guard at four participants", CampaignDifficulty::ExtraEnemies(4, 1, true, CampaignDifficulty::CatOrDog), 0);
    ensure_equals("ordinary bosses gain their first guard at six participants", CampaignDifficulty::ExtraEnemies(6, 1, true, CampaignDifficulty::CatOrDog), 1);
}

template<> template<> void campaign_test::test<5>() {
    const auto rabbit = CampaignDifficulty::ForPlayers(4, false, CampaignDifficulty::Rabbit, true, false);
    const auto wolf = CampaignDifficulty::ForPlayers(4, true, CampaignDifficulty::Wolf, true, false);
    ensure("ordinary rabbit stat growth is fifteen percent gentler", std::fabs(rabbit.vitality - 1.54825f) < 0.00001f);
    ensure("wolf boss stat growth is fifteen percent gentler", std::fabs(wolf.vitality - 3.4735f) < 0.00001f);
    ensure_equals("three extra allies buy only one rabbit reinforcement",
                  CampaignDifficulty::ExtraEnemies(4, 4, false, CampaignDifficulty::Rabbit), 1);
    ensure_equals("one helper does not add a mob even to a large crowd", CampaignDifficulty::ExtraEnemies(2, 30, false, CampaignDifficulty::Rat), 0);
    ensure_equals("two helpers still do not add a mob", CampaignDifficulty::ExtraEnemies(3, 30, false, CampaignDifficulty::Rabbit), 0);
    ensure_equals("a large original crowd cannot multiply the addition budget", CampaignDifficulty::ExtraEnemies(4, 30, false, CampaignDifficulty::Rat), 1);
    ensure_equals("each three additional weak-species allies buy one more mob", CampaignDifficulty::ExtraEnemies(7, 4, false, CampaignDifficulty::Rabbit), 2);
    ensure_equals("strong-species groups require four allies for one mob", CampaignDifficulty::ExtraEnemies(5, 4, false, CampaignDifficulty::CatOrDog), 1);
    ensure_equals("chapter addition cap remains bounded", CampaignDifficulty::kLevelExtraCap, 8);
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
}
