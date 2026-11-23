// Host-authoritative progression events; native state deduplicates each logical
// objective across retries and keeps deaths until that objective is won.
string ChallengeCampaignEncounterKey(int hotspot_id, int goal) {
    return "campaign:" + hotspot_id + ":" + goal;
}

void ChallengeCampaignGoalBegin(int hotspot_id, int goal, const string &in description) {
    if(Online_IsClient() || EditorModeActive()) return;
    TokenIterator tokens;
    tokens.Init();
    if(tokens.FindNextToken(description)) {
        string kind = tokens.GetToken(description);
        if(kind == "defeat" || kind == "spawn_defeat" || kind == "defeat_optional") {
            Online_ChallengeEncounterBegin(ChallengeCampaignEncounterKey(hotspot_id, goal));
        }
    }
}

void ChallengeCampaignGoalWin(int hotspot_id, int goal) {
    if(!Online_IsClient() && !EditorModeActive()) {
        Online_ChallengeEncounterWin(ChallengeCampaignEncounterKey(hotspot_id, goal));
    }
}

float challenge_campaign_goal_next_poll = 0.0f;
void ChallengeCampaignGoalPoll(int hotspot_id, int goal, const string &in description) {
    if(the_time >= challenge_campaign_goal_next_poll) {
        challenge_campaign_goal_next_poll = the_time + 0.5f;
        ChallengeCampaignGoalBegin(hotspot_id, goal, description);
    }
}
