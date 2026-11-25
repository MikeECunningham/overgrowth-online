// Campaigns advance immediately unless a session mod supplies an intermission.
bool progression_transition_pending = false;
void FinishProgressionTransition() {}
void AdvanceCampaign(const string &in next_level) {
    if(Online_IsClient()) return;
    if(next_level != "") LoadLevelID(next_level);
    else SendLevelMessage("go_to_main_menu");
}
