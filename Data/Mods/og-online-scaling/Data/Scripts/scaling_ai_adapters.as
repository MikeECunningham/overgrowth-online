// Optional integrations. All tuning derives from the host's encounter snapshot.
const string recovery_hook = "Data/Scripts/SeansGetUp/getupafterknockdown.as";
bool overhaul = false;
bool dynamic_aggression = false;
float next_scan = 0.0f;

float TacticsBoost() {
    float pressure;
    int rate, humans;
    if (!Online_GetEnemyDifficultySample(pressure, rate, humans)) return 0.0f;
    float load = max(0.0f, pressure) + 0.10f * float(max(0, humans - 1));
    return (load / (1.0f + load)) * (0.25f * float(rate));
}

void Init(string level_name) {
    overhaul = ModOnlineAdapterActive("crxhvrd-overhaul");
    dynamic_aggression = ModOnlineAdapterActive("com-dynamicai");
    if (ModOnlineAdapterActive("seans-smarter-deaths")) {
        level.AdaptHookFloatParameter(recovery_hook, "MinKoTime");
        level.AdaptHookFloatParameter(recovery_hook, "MaxKoTime");
    }
}

// Smarter Deaths rolls its own random duration between two bounds at KO.
// Its separate WakeupTime pin takes precedence and stays map-authored.
bool AdaptLevelHookFloat(int id, const string &in key, float &out value) {
    float boost = TacticsBoost();
    if (boost <= 0.0f || !Online_IsDifficultyEnemy(id)) return false;
    ScriptParams@ source = ReadObjectFromID(id).GetScriptParams();
    if (source.HasParam("MinKoTime") && source.HasParam("MaxKoTime")) {
        value = source.GetFloat(key);
    } else {
        // The mod stores its live menu settings in hundredths of a second.
        string setting = key == "MinKoTime" ? "global_wakeup_time_min" : "global_wakeup_time_max";
        if (!level.GetHookFloat(recovery_hook, setting, value)) return false;
        value *= 0.01f;
    }
    if (value > 0.0f) value /= (1.0f + boost);
    return true;
}

const array<string> drives = {"p_aio_jump_kicks", "p_aio_air_combat", "p_aio_feints", "p_aio_throwing"};
const array<string> sliders = {"AI - Jump Kicks", "AI - Air Combat", "AI - Feints", "AI - Weapon Throwing"};

void ApplyOverhaul(MovementObject@ actor, float boost) {
    ScriptParams@ source = ReadObjectFromID(actor.GetID()).GetScriptParams();
    for (uint i = 0; i < drives.length(); ++i) {
        if (!source.HasParam(sliders[i]) || !actor.HasVar(drives[i])) continue;
        // Overhaul subsequently multiplies this input by its own F9 controls
        // and personality. Always start from the authored slider.
        float value = max(0.0f, min(1.0f, source.GetFloat(sliders[i]) * (1.0f + boost)));
        if (actor.GetFloatVar(drives[i]) != value) actor.SetFloatVar(drives[i], value);
    }
}

class WaitCounter {
    int last = -1;
    float credit = 0.0f;

    void Advance(MovementObject@ actor, const string &in name, int threshold, float boost, bool waiting) {
        int current = actor.GetIntVar(name);
        if (!waiting || current < last) credit = 0.0f;
        if (waiting && last >= 0 && current == last + 1 && current < threshold - 1) {
            credit += boost;
            if (credit >= 1.0f) {
                credit -= 1.0f;
                actor.SetIntVar(name, ++current);
            }
        }
        last = waiting ? current : -1;
    }
}

class Fighter {
    bool overhaul_applied = false;
    WaitCounter defense, group;
}
dictionary fighters;

void ScanFighters(float boost) {
    dictionary present;
    for (int i = 0; i < GetNumCharacters(); ++i) {
        MovementObject@ actor = ReadCharacter(i);
        int id = actor.GetID();
        string key = "" + id;
        Fighter@ state;
        fighters.get(key, @state);
        if (boost <= 0.0f || !Online_IsDifficultyEnemy(id)) {
            if (state !is null && state.overhaul_applied) ApplyOverhaul(actor, 0.0f);
            continue;
        }
        bool has_drives = overhaul && actor.HasVar(drives[0]);
        bool has_waits = dynamic_aggression && actor.HasVar("max_defense_wait") &&
                         actor.HasVar("group_fighting_wait") && actor.HasVar("defense_wait") && actor.HasVar("group_wait");
        if (!has_drives && !has_waits) continue;
        if (state is null) @state = Fighter();
        if (has_drives) {
            ApplyOverhaul(actor, boost);
            state.overhaul_applied = true;
        }
        if (has_waits) {
            // Credit time only when the mod itself advances a wait. Its random
            // patience rolls, attack selection and aggression feedback stay intact.
            int limit = actor.GetIntVar("max_defense_wait");
            state.defense.Advance(actor, "defense_wait", limit, boost, actor.GetIntVar("defense_wait") > 0);
            state.group.Advance(actor, "group_wait", limit + 2, boost, actor.GetBoolVar("group_fighting_wait"));
        }
        present.set(key, @state);
    }
    fighters = present;
}

void Update(int is_paused) {
    if (is_paused != 0 || (!overhaul && !dynamic_aggression) || the_time < next_scan) return;
    next_scan = the_time + 0.2f;
    float boost = TacticsBoost();
    if (boost > 0.0f || !fighters.isEmpty()) ScanFighters(boost);
}
