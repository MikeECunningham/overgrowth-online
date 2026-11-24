#include <Online/campaign_equipment.h>
#include <Online/campaign_difficulty.h>
#include <tut/tut.hpp>

namespace tut {
struct campaign_equipment_data {};
typedef test_group<campaign_equipment_data> campaign_equipment_group;
campaign_equipment_group campaign_equipment_tests("CampaignEquipment");
typedef campaign_equipment_group::object equipment_test;

template<> template<> void equipment_test::test<1>() {
    using namespace CampaignEquipment;
    ensure_equals(ArmoredFraction(0.3), 0.0);
    ensure("delayed onset is cubic", ArmoredFraction(0.32) / ArmoredFraction(0.31) > 7.9);
    ensure("armor approaches full coverage slowly", ArmoredFraction(1000) > ArmoredFraction(3) && ArmoredFraction(1000) < 1);
    ensure("late marginal growth falls away", ArmoredFraction(10) - ArmoredFraction(9) < ArmoredFraction(3) - ArmoredFraction(2));
    for (uint32_t seed = 0; seed < 5000; ++seed) {
        int previous = 0;
        for (double intensity : {0.0, 0.3, 0.5, 1.0, 1.5, 3.0, 1000.0}) {
            const auto plan = ForUnit(seed, intensity, true, true, false);
            ensure("fixed percentiles never reroll/remove pieces as difficulty grows", (plan.armor_regions & previous) == previous);
            ensure("only valid body regions", (plan.armor_regions & ~kAllRegions) == 0);
            previous = plan.armor_regions;
        }
    }
}

template<> template<> void equipment_test::test<2>() {
    using namespace CampaignEquipment;
    constexpr int count = 50000;
    int armored = 0, full = 0, pieces = 0;
    for (uint32_t seed = 0; seed < count; ++seed) {
        const auto plan = ForUnit(seed, 1.5, true, false, false);
        armored += plan.armor_regions != 0; full += plan.armor_regions == kAllRegions; pieces += Pieces(plan.armor_regions);
    }
    const double maturity = ArmoredFraction(1.5);
    ensure("many late enemies have armor", std::fabs(double(armored) / count - maturity) < 0.012);
    ensure("full suits remain rare at ordinary late-game intensity", double(full) / count > 0.003 && double(full) / count < 0.025);
    ensure("coverage follows the planned curve", std::fabs(double(pieces) / count - maturity * (1 + 8 * maturity)) < 0.1);
}

template<> template<> void equipment_test::test<3>() {
    using namespace CampaignEquipment;
    for (uint32_t seed = 0; seed < 10000; ++seed) {
        ensure_equals("wolves never receive weapons", int(ForUnit(seed, 20, true, true, true).weapon), int(Unarmed));
        ensure_equals("weapon checkbox off means unarmed", int(ForUnit(seed, 20, true, false, false).weapon), int(Unarmed));
        for (int slot = 0; slot < 6; ++slot) ensure_equals(int(WaveWeapon(seed, slot, 6, 20, true)), int(Unarmed));
    }
}

template<> template<> void equipment_test::test<4>() {
    using namespace CampaignEquipment;
    for (uint32_t seed = 0; seed < 1000; ++seed) for (int count = 1; count <= 6; ++count) {
        for (double intensity : {0.0, 0.4, 0.8, 1.5, 3.0}) {
            int armed = 0;
            for (int slot = 0; slot < count; ++slot) {
                const auto weapon = WaveWeapon(seed, slot, count, intensity, false);
                armed += weapon != Unarmed;
                if (intensity < 1.15) ensure("early/mid equipment consists of single daggers", weapon == Unarmed || weapon == Dagger);
            }
            ensure_equals("wave allocation obeys its bounded armed-enemy budget", armed, ArmedCount(count, intensity));
            if (intensity == 0.4) ensure_equals("one early-mid extra has a dagger", armed, 1);
            if (intensity == 0.8) ensure("two mid extras can have one dagger each", armed >= std::min(2, count));
        }
    }
}

template<> template<> void equipment_test::test<5>() {
    using namespace CampaignEquipment;
    const auto original = ExtraSeed(363, 14, 2, 1);
    ensure_equals("retry identities do not depend on newly allocated clone object IDs", ExtraSeed(363, 14, 2, 1), original);
    ensure("distinct wave slots receive distinct identities", ExtraSeed(363, 14, 2, 2) != original);
    ensure("different encounters don't repeat a wave plan", ExtraSeed(363, 14, 4, 1) != original);
    for (int humans = 1; humans <= 64; ++humans) {
        ensure_equals("armor never becomes a passive-block multiplier", CampaignDifficulty::ForParty(humans, 7, true, CampaignDifficulty::Wolf, true, false).armor, 1.0f);
    }
    ensure_equals(CampaignDifficulty::AIAmount(1, 0.59), 0.0f);
    ensure_equals(CampaignDifficulty::AIAmount(1, 0.6), 0.01f);
    ensure_equals(CampaignDifficulty::AIAmount(1, 1.1), 0.02f);
    ensure_equals(CampaignDifficulty::AIAmount(1, 1.5), 0.03f);
    ensure_equals(CampaignDifficulty::AIAmount(4, 0), 0.01f);
}
template<> template<> void equipment_test::test<6>() {
    using CampaignEquipment::RetargetAnchor;
    ensure("reference fit is unchanged even with a distinct physical COM",std::fabs(RetargetAnchor(.531875f,.535144f,.6f,1.12f,.535144f,.6f,1.12f)-.531875f)<1e-6f);
    const float shifted=RetargetAnchor(.531875f,.535144f,.535144f,1.12f,.618965f,.65f,.9f);
    ensure("scale is applied around physical inverse-bind COM",std::fabs(shifted-(.65f+(.618965f+(.531875f-.535144f)/1.12f-.65f)*.9f))<1e-6f);
    ensure("canonical Turner 1x appearance is unchanged",std::fabs(RetargetAnchor(.626611f,.535144f,.535144f,1,.535144f,.6f,1)-.626611f)<1e-6f);
    const float twice=RetargetAnchor(.626611f,.535144f,.535144f,1,.535144f,.6f,2);
    ensure("animated bone plus attachment produces the doubled point",std::fabs(.6f+twice-2*.626611f)<1e-6f);
    const float wolf=RetargetAnchor(.7f,.5f,.55f,1,.6f,.65f,1.8f,1.8f/1.3f);
    ensure("model-scaled skeleton and character-scaled gear offsets stay separate",std::fabs((1.8f/1.3f-1)*.65f+wolf-(.6f*1.8f/1.3f+(.7f-.5f)*1.8f))<1e-6f);
}
}
