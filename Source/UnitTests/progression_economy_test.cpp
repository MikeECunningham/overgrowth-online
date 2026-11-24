#include <Online/progression_economy.h>
#include <tut/tut.hpp>

namespace tut {
struct xp_economy_data {};
typedef test_group<xp_economy_data> xp_group;
xp_group xp_tests("FriendlyProgression");
typedef xp_group::object xp_test;
template<> template<> void xp_test::test<1>() {
    using namespace FriendlyProgression;
    Wallet wallet;
    wallet.Earn(16, false);
    ensure_equals(wallet.held, 0.);
    wallet.Commit();
    wallet.Commit();
    ensure_equals(wallet.commit_serial, 1u);
    ensure_equals(wallet.held, 16.);
    ensure_equals(wallet.pooled, 0.);
    wallet.Earn(-30, true);
    ensure_equals(wallet.held, 0.);
}
template<> template<> void xp_test::test<2>() {
    using namespace FriendlyProgression;
    ensure("one ordinary Arena victory affords a starting stat", Reward(0,false,.5,0,1,1,0) >= StatPrice(0));
    ensure("spawn weapons and physical armor raise the bounty", Reward(0,false,.5,.5,1,1,0) > Reward(0,false,.5,0,1,1,0));
    ensure("effective enemy scaling raises the reward", Reward(3,false,.5,.5,2,1,.25) > Reward(3,false,.5,.5,1,1,0));
    for (unsigned slot = 0; slot < 9; ++slot) {
        int tier = ArmorProtection::NextShopMaterial(slot, 0);
        ensure_equals(tier, int(slot < 3 ? ArmorProtection::Chitin : ArmorProtection::Wood));
        for (; tier < ArmorProtection::Grey; ++tier)
            ensure_equals(ArmorProtection::NextShopMaterial(slot, tier), tier + 1);
        ensure_equals(ArmorProtection::NextShopMaterial(slot, tier), 0);
    }
    ensure_equals(ArmorProtection::RestoredShopMaterial(3, 2), int(ArmorProtection::Wood));
    ensure_equals(ArmorProtection::RestoredShopMaterial(2, 1), int(ArmorProtection::Dog));
    ensure_equals(ArmorProtection::RestoredShopMaterial(3, 2, false), int(ArmorProtection::Chitin));
    ensure_equals(ArmorPrice(3, ArmorProtection::Wood), 155);
    ensure_equals(ArmorPrice(3, ArmorProtection::Chitin), 310);
    ensure_equals(ArmorPrice(3, ArmorProtection::Dog), 465);
    ensure_equals(ArmorPrice(3, ArmorProtection::Grey), 775);
    ensure("recruit prices more than double for this team", RecruitPrice(2) > 2 * RecruitPrice(1));
    ensure("stat marginal price accelerates", StatPrice(5)-StatPrice(4) > StatPrice(1)-StatPrice(0));
}
}
