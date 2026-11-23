// Each match gets an engine-time token: the_time survives level retries, while
// each script's globals may reset. Roster changes may reset the displayed round
// number, so challenge rounds use their own sequence within that match.
string challenge_arena_match_key;
string challenge_arena_round_key;
int challenge_arena_round_serial = 0;

void ChallengeArenaNewMatch() {
    if(Online_IsClient() || EditorModeActive()) return;
    challenge_arena_match_key = "arena_match:" + the_time;
    challenge_arena_round_key = "";
    challenge_arena_round_serial = 0;
}

void ChallengeArenaRoundBegin() {
    if(Online_IsClient() || EditorModeActive()) return;
    if(challenge_arena_match_key == "") ChallengeArenaNewMatch();
    ++challenge_arena_round_serial;
    challenge_arena_round_key = challenge_arena_match_key + ":round:" + challenge_arena_round_serial;
    Online_ChallengeEncounterBegin(challenge_arena_round_key);
}

void ChallengeArenaRoundWin(int winner) {
    if(Online_IsClient() || EditorModeActive() || challenge_arena_round_key == "") return;
    int reference = Online_ChallengeReferenceAvatar();
    if(MovementObjectExists(reference) && MovementObjectExists(winner) &&
       ReadCharacterID(reference).OnSameTeam(ReadCharacterID(winner))) {
        Online_ChallengeEncounterWin(challenge_arena_round_key);
    }
}

void ChallengeArenaMatchComplete() {
    if(!Online_IsClient() && !EditorModeActive() && challenge_arena_match_key != "") {
        Online_ChallengeCompletion(challenge_arena_match_key);
    }
}
