#include <Online/progression_economy.h>
#include <Online/friendly_progression.h>
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
    wallet.Earn(99999999., true);
    wallet.Earn(16., false);
    wallet.Commit();
    ensure_equals("bank and checkpoint earnings cross 100 million", wallet.held, 100000015.);
    wallet.Earn(4294967296., false);
    wallet.Commit();
    ensure_equals("balances and commit amounts support more than 32 bits", wallet.held, 4394967311.);
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
template<> template<> void xp_test::test<3>() {
    Json::Value snapshot(Json::objectValue);
    snapshot["v"] = 2;
    snapshot["shop"] = Json::UInt(0); snapshot["attempt"] = Json::UInt(0);
    snapshot["open"] = snapshot["final"] = snapshot["loop"] = false;
    snapshot["ms_left"] = -1.0;
    snapshot["ready"] = snapshot["players"] = snapshot["teams"] = Json::Value(Json::arrayValue);
    ensure("older snapshots may omit derived scales", FriendlyProgression::Restore(snapshot, true));
    for (Json::Value invalid : {Json::Value(), Json::Value(1), Json::Value("scale"), Json::Value(Json::objectValue)}) {
        snapshot["xp_scales"] = invalid;
        ensure("present scale containers must be arrays", !FriendlyProgression::Restore(snapshot, true));
    }
    for (Json::Value invalid : {Json::Value(), Json::Value(1), Json::Value("row"), Json::Value(Json::arrayValue)}) {
        snapshot["xp_scales"] = Json::Value(Json::arrayValue);
        snapshot["xp_scales"].append(invalid);
        ensure("scalar scale rows fail without throwing", !FriendlyProgression::Restore(snapshot, true));
    }
    snapshot["xp_scales"] = Json::Value(Json::arrayValue);
    snapshot["xp_scales"].resize(8193);
    ensure("oversized arrays are rejected", !FriendlyProgression::Restore(snapshot, true));
    Json::Value row;
    row["player"] = Json::UInt(3); row["world_remaining"] = 100.0;
    row["encounter_expected"] = 32.0; row["encounter_label"] = "Current encounter";
    snapshot["xp_scales"] = Json::Value(Json::arrayValue);
    snapshot["xp_scales"].append(row);
    ensure("bounded scales pass preflight", FriendlyProgression::Restore(snapshot, true));
    snapshot["xp_scales"].append(row);
    ensure("duplicate player scales are rejected", !FriendlyProgression::Restore(snapshot, true));
    snapshot["xp_scales"].resize(1);
    snapshot["xp_scales"][0]["world_remaining"] = -1;
    ensure("negative reward estimates are rejected", !FriendlyProgression::Restore(snapshot, true));
}
}
