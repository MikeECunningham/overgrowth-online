// A numbered objective becomes a respawn checkpoint only if the level has
// assigned a real player spawn marker to that step.
bool HasPlayerSpawnCheckpoint(int progress) {
    if(progress <= 0 || !params.HasParam("player_spawn")) return false;
    string spawns = params.GetString("player_spawn");
    TokenIterator markers;
    markers.Init();
    for(int step = 1; step <= progress; ++step) {
        if(!markers.FindNextToken(spawns)) return false;
    }
    int id = atoi(markers.GetToken(spawns));
    return ObjectExists(id) && ReadObjectFromID(id).GetType() == _placeholder_object;
}
