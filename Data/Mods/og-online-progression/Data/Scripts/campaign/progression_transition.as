// The campaign script survives the level change. Defer the already resolved
// destination until every shopping seat is ready or the host's timer expires.
bool progression_transition_pending = false;
string progression_destination;

void FinishProgressionTransition() {
    if(Online_IsClient()) return;
    string saved_destination;
    if(SessionCampaignTransition(saved_destination)) {
        progression_transition_pending = true;
        progression_destination = saved_destination;
    }
    if(!progression_transition_pending || ProgressionShopOpen()) return;
    progression_transition_pending = false;
    SessionSetCampaignTransition(false, "");
    if(progression_destination != "") {
        LoadLevelID(progression_destination);
    } else if(ProgressionLoopChosen()) {
        array<ModLevel>@ levels = GetCampaign(GetCurrCampaignID()).GetLevels();
        if(levels.length() > 0) {
            ProgressionCampaignLoop();
            LoadLevelID(levels[0].GetID());
        } else {
            SendLevelMessage("go_to_main_menu");
        }
    } else {
        SendLevelMessage("go_to_main_menu");
    }
}

void AdvanceCampaign(const string &in next_level) {
    if(progression_transition_pending || Online_IsClient()) return;
    ProgressionCampaignComplete();
    if(ProgressionBeginShop(next_level == "")) {
        progression_destination = next_level;
        progression_transition_pending = true;
        SessionSetCampaignTransition(true, next_level);
    } else if(next_level != "") {
        LoadLevelID(next_level);
    } else {
        SendLevelMessage("go_to_main_menu");
    }
}
