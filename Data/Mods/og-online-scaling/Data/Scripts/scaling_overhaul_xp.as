// Overhaul supplies its own character script. Feed its observed health and KO
// transitions into the progression ledger on the machine that owns the fighter.
class FighterVitals {
    MovementObject@ actor;
    float health, blood;
    int knocked_out;
}

dictionary vitals;
bool use_overhaul = false;

void Init(string level_name) {
    use_overhaul = ModOnlineAdapterActive("crxhvrd-overhaul");
}

void Observe(MovementObject@ actor, bool defeat_message = false) {
    const int id = actor.GetID();
    const string key = "" + id;
    if(actor.network_proxy || !ReadObjectFromID(id).GetEnabled()) {
        vitals.delete(key);
        return;
    }
    FighterVitals@ before;
    vitals.get(key, @before);
    bool fresh = before is null || !(before.actor is actor);
    if(fresh) {
        // Other character scripts in the same map can report their own hits.
        if(actor.HasVar("progression_wounds") || !actor.HasVar("aio_npc_air_attacks")) return;
        @before = FighterVitals();
        @before.actor = actor;
        vitals.set(key, @before);
    }
    const float health = max(0.0f, min(1.0f, actor.GetFloatVar("temp_health")));
    const float blood = max(0.0f, min(1.0f, actor.GetFloatVar("blood_health")));
    const int ko = actor.GetIntVar("knocked_out");
    if(fresh && defeat_message && ko != 0) {
        // Ownership can transfer for a throw or choke before our first sample.
        ProgressionReportHit(id, actor.GetIntVar("attacked_by_id"), 0.0f, true);
    } else if(!fresh && before.knocked_out == 0) {
        // Blood loss can reduce both bars for the same wound.
        const float damage = max(before.health - health, before.blood - blood);
        const int attacker = actor.GetIntVar("attacked_by_id");
        if(damage > 0.0f) ProgressionReportHit(id, attacker, damage, false);
        if(ko != 0) ProgressionReportHit(id, attacker, 0.0f, true);
    } else if(!fresh && ko == 0) {
        ProgressionReportRecovery(id);
    }
    before.health = health;
    before.blood = blood;
    before.knocked_out = ko;
}

void Update(int paused) {
    if(!use_overhaul || !ProgressionEnabled() || paused != 0) return;
    for(int i = 0; i < GetNumCharacters(); ++i) Observe(ReadCharacter(i));
    if(vitals.getSize() > uint(GetNumCharacters())) {
        array<string> keys = vitals.getKeys();
        for(uint i = 0; i < keys.length(); ++i) {
            if(!MovementObjectExists(atoi(keys[i]))) vitals.delete(keys[i]);
        }
    }
}

void ReceiveMessage(string message) {
    if(!use_overhaul || !ProgressionEnabled()) return;
    if(message == "reset" || message == "post_reset") {
        vitals.deleteAll();
        return;
    }
    // A round can finish before the next update. Count the finishing hit while
    // the defeated fighter and its attacker still belong to this round.
    TokenIterator tokens;
    tokens.Init();
    if(!tokens.FindNextToken(message)) return;
    string event = tokens.GetToken(message);
    if(event != "character_died" && event != "character_knocked_out") return;
    if(!tokens.FindNextToken(message)) return;
    int id = atoi(tokens.GetToken(message));
    if(MovementObjectExists(id)) Observe(ReadCharacterID(id), true);
}
