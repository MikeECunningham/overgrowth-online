// Host-run Survival level script. Apache-2.0.
const string mod_id = "og-online-survival";
const float wave_interval = 20.0f;
const float battery_break = 12.0f;
const int deployment_seconds = 4;
const float finish_camera_time = 3.0f;
const float scoreboard_page_seconds = 5.0f;
const int first_armed_battery = 4;
const array<string> actors = {
    "Data/Objects/characters/rabbits/male_rabbit_1_actor.xml",
    "Data/Objects/characters/rats/rat_actor.xml",
    "Data/Objects/characters/raider_rabbit_actor.xml",
    "Data/Objects/characters/cats/cat_actor.xml",
    "Data/Objects/characters/dogs/dog_soldier_actor.xml",
    "Data/Objects/characters/wolves/male_wolf_actor.xml"
};
const array<string> weapons = {"Data/Items/flint_knife.xml", "Data/Items/staffbasic.xml",
    "Data/Items/Rapier.xml", "Data/Items/Bastard.xml"};

enum Phase { preparing, fighting, resting, defeated, victory, results }
Phase phase = preparing;
int battery = 1;
int launched = 0;
int wave_count = 1;
float due = 0, next_tick = 0, next_publish = 0;
int last_countdown_second = 0;
bool host_ready = false;
bool awaiting_deployment = false;
vec3 home;
array<int> teams, eliminated, winning_slots;
class TeamScore {
    int team, waves, kills;
    int peak_members = 0;
    float last_contact = 0;
}
array<TeamScore@> scores;
TeamScore@ ScoreFor(int team) {
    for (uint i = 0; i < scores.length(); ++i)
        if (scores[i].team == team) return scores[i];
    TeamScore@ score = TeamScore(); score.team = team;
    scores.insertLast(score);
    return score;
}
class Raider {
    int team, slot, kind;
    int weapon = -1;
    int id = -1;  // Pending a safe spawn or reconstruction from a save.
    int spawn_attempt = 0;
    float retry_at = 0;
    int staged_actor = -1, staged_weapon = -1;
}
class Forecast {
    int battery, wave, team, members, seed;
    array<Raider@> fighters;
}
array<Forecast@> forecasts;
class Wave {
    int number;
    array<int> teams;
    array<Raider@> remaining;
}
array<Wave@> waves;
array<int> corpses;
array<float> remove_at;
int text_id = -1, score_id = -1;
string status_text, score_text;
float status_width = 0, status_height = 0;
int score_height = 0;
int last_result_press = -2;
float next_hud = -1.0f;
string hud_snapshot;
JSONValue hud_state;
float hud_due = 0;

class NeutralMarker {
    int screen, actor;
    bool locator;
}
array<NeutralMarker> neutral_markers;

bool IsNeutralWaveEnemy(MovementObject@ actor, int team) {
    return actor.OnTeam("survival_enemies") && actor.OnTeam("lobby_team_" + team);
}

vec3 MarkerPoint(MovementObject@ actor) {
    return actor.rigged_object().GetIKChainPos("head", 1) + vec3(0, 0.30f, 0);
}

float MarkerSize() {
    return min(20.0f, max(12.0f, GetScreenHeight() / 55.0f));
}

void UpdateNeutralMarkers() {
    neutral_markers.resize(0);
    if (hud_state["phase"].asInt() != fighting) return;
    array<int> searching = ReadNumbers(hud_state["searching"]);
    array<LobbyPlayer>@ roster = LobbyPlayers();
    for (int i = 0; i < GetNumCharacters(); ++i) {
        MovementObject@ actor = ReadCharacter(i);
        if (!actor.OnTeam("survival_enemies") || !Up(actor.GetID())) continue;
        vec3 point = MarkerPoint(actor);
        for (uint j = 0; j < roster.length(); ++j) {
            if (!roster[j].local || roster[j].bot) continue;
            bool neutral = IsNeutralWaveEnemy(actor, roster[j].team);
            if (!neutral && searching.find(roster[j].team) < 0) continue;
            vec2 pixel;
            if (!ProjectToLocalScreen(roster[j].screen, point, MarkerSize() * 0.5f, pixel, neutral)) continue;
            NeutralMarker marker;
            marker.locator = !neutral;
            marker.screen = roster[j].screen;
            marker.actor = actor.GetID();
            neutral_markers.insertLast(marker);
        }
    }
}

void DrawNeutralMarkers() {
    float size = MarkerSize();
    for (uint i = 0; i < neutral_markers.length(); ++i) {
        int id = neutral_markers[i].actor;
        if (!Up(id)) continue;
        vec2 pixel;
        if (!ProjectToLocalScreen(neutral_markers[i].screen, MarkerPoint(ReadCharacterID(id)), size * 0.5f, pixel, !neutral_markers[i].locator)) continue;
        HUDImage@ icon = hud.AddImage();
        icon.SetImageFromPath("Data/Images/survival-neutral.png");
        if (neutral_markers[i].locator) icon.color = vec4(1, 0.28f, 0.12f, 1);
        icon.position = vec3(pixel.x - size * 0.5f, pixel.y - size * 0.5f, 2);
        icon.scale = vec3(size / icon.GetWidth(), size / icon.GetHeight(), 1);
    }
}

void CountdownTick() {
    int second = int(ceil(due - the_time));
    // The final four seconds of a battery break are the deployment phase.
    // Announce the approaching first wave just once across that transition.
    if (phase != preparing && !(phase == fighting && launched < wave_count)) second = 0;
    if (second < 1 || second > 3) second = 0;
    if (second != 0 && second != last_countdown_second)
        level.SendMessage("ohl survival countdown " + second);
    last_countdown_second = second;
}

int WavesForBattery(int number) {
    int count = 1;
    for (int threshold = 2; count < 7 && number >= threshold; threshold *= 2) ++count;
    return count;
}
string BatteryKey() { return "survival:" + GetCurrLevelRelPath() + ":battery:" + battery; }
string WaveKey(int wave) { return BatteryKey() + ":wave:" + wave; }

bool Up(int id) {
    return MovementObjectExists(id) && ReadObjectFromID(id).GetEnabled() &&
           ReadCharacterID(id).GetIntVar("knocked_out") == 0;
}

bool FloorBelow(vec3 anchor, float height, vec3 &out spot) {
    vec3 floor = col.GetRayCollision(anchor + vec3(0, height, 0), anchor - vec3(0, 200, 0));
    return Online_FindSafeSpot(floor + vec3(0, 0.9f, 0), vec3(0, 0, 1), 0, spot);
}

void Init(string level_name) {
    text_id = level.CreateTextElement();
    level.GetTextElement(text_id).Create(512, 80);
    score_id = level.CreateTextElement();
    level.GetTextElement(score_id).Create(512, 512);

    // Authored opponents and mission goals belong to the source map's game mode.
    // Keep its geometry, weapons, spawn locations, and environmental hazards.
    array<int>@ hotspots = GetObjectIDsType(_hotspot_object);
    for (uint i = 0; i < hotspots.length(); ++i) {
        Object@ o = ReadObjectFromID(hotspots[i]);
        if (o.GetScriptParams().HasParam("goal_0") ||
            cast<Hotspot@>(o).HasFunction("string SessionCheckpointState()"))
            o.SetEnabled(false);
    }

    bool has_player = false;
    for (int i = 0; i < GetNumCharacters(); ++i) {
        MovementObject@ mo = ReadCharacter(i);
        Object@ o = ReadObjectFromID(mo.GetID());
        if (mo.is_player) {
            if (!has_player) home = o.GetTranslation();
            has_player = true;
        } else if (!o.GetScriptParams().HasParam("Survival Team") && !Online_IsPlayerCharacter(mo.GetID())) {
            if (!has_player) home = o.GetTranslation();
            if (Online_IsHosting()) o.SetEnabled(false);
        }
    }
    if (!Online_IsHosting()) return;
    if (!has_player) {
        // A custom freeplay map may only have a camera or a plain spawn point.
        if (GetNumCharacters() == 0) {
            array<int>@ anchors = GetObjectIDsType(_spawn_point);
            if (anchors.length() == 0) @anchors = GetObjectIDsType(_camera_type);
            if (anchors.length() > 0) home = ReadObjectFromID(anchors[0]).GetTranslation();
        }
        array<int>@ markers = GetObjectIDsType(_placeholder_object);
        for (uint i = 0; i < markers.length(); ++i) {
            Object@ marker = ReadObjectFromID(markers[i]);
            ScriptParams@ p = marker.GetScriptParams();
            if (!p.HasParam("Name")) continue;
            string name = p.GetString("Name");
            if (name == "arena_spawn" || name == "player_spawn" || name == "character_spawn") {
                home = marker.GetTranslation();
                break;
            }
        }
        vec3 spot;
        // Prefer the marker's own floor in covered arenas. Suspended cameras
        // or an empty starter's origin below its terrain need a longer ray.
        if (Online_FindSafeSpot(home, vec3(0, 0, 1), 0, spot) ||
            FloorBelow(home, 0.5f, spot) || FloorBelow(home, 500.0f, spot)) {
            home = spot;
        }
        string path = "Data/Objects/IGF_Characters/IGF_TurnerActor.xml";
        int id = CreateObject(path, true);
        if (MovementObjectExists(id)) {
            ReadObjectFromID(id).SetTranslation(home);
            ReadObjectFromID(id).SetPlayer(true);
            Online_AnnounceObject(id, path);
        }
    }
}

array<int> RosterTeams(const array<PlayerState> &in seats) {
    array<int> result;
    for (uint i = 0; i < seats.length(); ++i)
        if (MovementObjectExists(seats[i].avatar_id) && result.find(seats[i].team) < 0)
            result.insertLast(seats[i].team);
    return result;
}

array<int> Members(const array<PlayerState> &in seats, int team, bool only_up) {
    array<int> members;
    for (uint i = 0; i < seats.length(); ++i) {
        int id = seats[i].avatar_id;
        if (seats[i].team == team && MovementObjectExists(id) && (!only_up || Up(id))) members.insertLast(id);
    }
    return members;
}

array<int> Winners(const array<PlayerState> &in seats, const array<int> &in participants) {
    array<int> winners;
    for (uint i = 0; i < seats.length(); ++i) {
        if (participants.find(seats[i].team) >= 0 && eliminated.find(seats[i].team) < 0 &&
            MovementObjectExists(seats[i].avatar_id)) winners.insertLast(seats[i].avatar_id);
    }
    return winners;
}

string EnemyTags(int target, const array<int> &in present) {
    string tags = "survival_enemies";
    for (uint t = 0; t < present.length(); ++t)
        if (present[t] != target) tags += ",lobby_team_" + present[t];
    return tags;
}

void SetTags(int id, const string &in tags) {
    Object@ object = ReadObjectFromID(id);
    ScriptParams@ params = object.GetScriptParams();
    if (params.HasParam("Teams") && params.GetString("Teams") == tags) return;
    params.SetString("Teams", tags);
    object.UpdateScriptParams();
}

vec2 SpawnRadii(int number) {
    float growth = float(min(20, max(1, number)) - 1) / 19.0f;
    return mix(vec2(12, 20), vec2(40, 64), growth);
}

bool FindApproach(vec3 spot, vec3 anchor, vec3 &out rally) {
    NavPath route = GetPath(spot, anchor);
    float span = sqrt(xz_distance_squared(spot, anchor));
    float stand_off = min(6.0f, span / 3.0f);
    if (route.NumPoints() != 1) {
        if (!route.success || route.NumPoints() < 2) return false;
        // Stop short along the walkable route. A hidden, stationary fighter
        // should be able to let the search party pass without being discovered.
        rally = route.GetPoint(route.NumPoints() - 1);
        for (int i = route.NumPoints() - 2; i >= 0; --i) {
            vec3 previous = route.GetPoint(i);
            float leg = distance(previous, rally);
            if (leg > stand_off) {
                rally = mix(rally, previous, stand_off / leg);
                break;
            }
            stand_off -= leg;
            rally = previous;
        }
        return true;
    }
    // Without navigation data the engine returns just the requested endpoint.
    // Check the straight approach for walls, missing ground and changes of floor.
    if (distance_squared(route.GetPoint(0), anchor) > 0.01f) return false;
    int steps = max(1, int(ceil(span / 2.0f)));
    vec3 previous = spot;
    rally = spot;
    for (int i = 1; i <= steps; ++i) {
        vec3 sample = mix(spot, anchor, float(i) / float(steps));
        sample.y = previous.y;
        vec3 floor = col.GetRayCollision(sample + vec3(0, 1, 0), sample - vec3(0, 2.5f, 0));
        vec3 next = floor + vec3(0, 0.8f, 0);
        if (abs(next.y - previous.y) > 1.3f ||
            distance_squared(col.GetRayCollision(previous, next), next) > 0.01f) return false;
        if (float(steps - i) * span / float(steps) >= stand_off) rally = next;
        previous = next;
    }
    return abs(previous.y - anchor.y) < 1.5f;
}

bool SpawnSpot(const array<int> &in targets, vec3 anchor, vec2 radii, int misses, vec3 &out pos, vec3 &out rally, uint seed) {
    // A confined map can have no reachable ground in the preferred ring.
    // On successive director ticks search adjacent rings, always checking the final footprint.
    for (int i = 0; i < misses && radii.x > 1.8f; ++i) {
        radii.y = radii.x;
        radii.x = max(1.8f, radii.x * 0.55f);
    }
    float near_sq = radii.x * radii.x, far_sq = radii.y * radii.y;
    WaveRandom random;
    random.state = seed;
    float heading = random.Fraction() * 6.2831853f;
    for (int i = 0; i < 8; ++i) {
        float angle = heading + float(i) * 0.7853982f;
        vec3 dir(sin(angle), 0, cos(angle));
        float radius = sqrt(mix(near_sq, far_sq, random.Fraction()));
        vec3 candidate = anchor + dir * radius;
        NavPoint nav = GetNavPoint(candidate);
        if (nav.IsSuccess()) candidate = nav.GetPoint() + vec3(0, 0.8f, 0);
        if (!Online_FindSafeSpot(candidate, dir * -1.0f, 0, pos)) {
            // A distant hillside can be well above or below the anchor's floor.
            float rise = radius * 0.65f;
            vec3 end = candidate - vec3(0, rise, 0);
            vec3 floor = col.GetRayCollision(candidate + vec3(0, rise, 0), end);
            if (distance_squared(floor, end) < 0.01f ||
                !Online_FindSafeSpot(floor + vec3(0, 0.8f, 0), dir * -1.0f, 0, pos)) continue;
        }
        if (xz_distance_squared(pos, anchor) > far_sq) continue;
        bool clear = true;
        for (uint n = 0; n < targets.length() && clear; ++n)
            clear = xz_distance_squared(pos, ReadCharacterID(targets[n]).position) >= near_sq;
        for (int n = 0; n < GetNumCharacters() && clear; ++n) {
            MovementObject@ other = ReadCharacter(n);
            clear = !ReadObjectFromID(other.GetID()).GetEnabled() || distance_squared(other.position, pos) >= 1.0f;
        }
        if (clear && FindApproach(pos, anchor, rally)) return true;
    }
    return false;
}

// One point buys a rat. The point budget rises within every battery; a new
// battery can start below the previous finale. Creature and weapon costs are
// independent of optional AI/stat scaling, which is sampled at deployment.
const array<int> species_cost = {2, 1, 2, 5, 11, 22};
const array<int> weapon_cost = {1, 2, 3, 5};
const int max_wave_fighters = 10;

int WaveBudget(int number, int wave, int members) {
    int base = min(80, max(1, number));
    int step = 1 + min(3, (base - 1) / 10);
    return base + (wave - 1) * step + min(4, max(0, members - 1)) * min(4, base);
}

uint MixSeed(uint x) {
    x = (x ^ (x >> 16)) * 0x7feb352d;
    x = (x ^ (x >> 15)) * 0x846ca68b;
    return x ^ (x >> 16);
}

class WaveRandom {
    uint state;
    uint Next() { state += 0x9e3779b9; return MixSeed(state); }
    float Fraction() { return float(Next() >> 8) / 16777216.0f; }
}

uint WaveSeed(int seed, int number, int wave) {
    return MixSeed(MixSeed(MixSeed(uint(seed)) + uint(number)) + uint(wave));
}

class WaveOffer {
    int kind, weapon, cost, weight;
}

array<Raider@> PlanWave(int seed, int number, int wave, int members, int team) {
    int budget = WaveBudget(number, wave, members);
    WaveRandom random;
    // Equally sized teams face the same draw regardless of roster iteration order.
    random.state = WaveSeed(seed, number, wave) + uint(members);
    array<WaveOffer> offers;
    for (int kind = 0; kind < int(actors.length()); ++kind) {
        if (kind == 2 || (kind == 3 && number < first_armed_battery) ||
            (kind == 4 && number < 8) || (kind == 5 && number < 16)) continue;
        for (int item = -1; item < int(weapons.length()); ++item) {
            if (item >= 0 && (number < first_armed_battery || kind == 5)) break;
            WaveOffer offer;
            offer.kind = kind; offer.weapon = item;
            offer.cost = species_cost[kind] + (item < 0 ? 0 : weapon_cost[item]);
            offer.weight = item < 0 ? 6 : 1;
            if (offer.cost <= budget) offers.insertLast(offer);
        }
    }
    // Fewest fighters able to spend each remainder exactly. A random purchase
    // remains eligible only if the rest still fits in this team's actor limit.
    array<int> minimum(budget + 1, max_wave_fighters + 1);
    minimum[0] = 0;
    for (int points = 1; points <= budget; ++points)
        for (uint i = 0; i < offers.length(); ++i)
            if (offers[i].cost <= points)
                minimum[points] = min(minimum[points], 1 + minimum[points - offers[i].cost]);

    array<Raider@> plan;
    while (budget > 0) {
        int slots = max_wave_fighters - int(plan.length());
        int weight = 0;
        for (uint i = 0; i < offers.length(); ++i) {
            int left = budget - offers[i].cost;
            if (left >= 0 && minimum[left] < slots) weight += offers[i].weight;
        }
        if (weight == 0) break;
        int draw = int(random.Next() % uint(weight));
        for (uint i = 0; i < offers.length(); ++i) {
            WaveOffer@ offer = offers[i];
            int left = budget - offer.cost;
            if (left < 0 || minimum[left] >= slots) continue;
            draw -= offer.weight;
            if (draw >= 0) continue;
            Raider@ r = Raider();
            r.team = team; r.slot = int(plan.length());
            r.kind = offer.kind == 0 && (random.Next() & 1) != 0 ? 2 : offer.kind;
            r.weapon = offer.weapon;
            plan.insertLast(r);
            budget = left;
            break;
        }
    }
    return plan;
}

void DeleteStaged(int id) {
    if (id < 0 || !ObjectExists(id)) return;
    Object@ object = ReadObjectFromID(id);
    ScriptParams@ params = object.GetScriptParams();
    if (!object.GetEnabled() && params.HasParam("Survival Staged") && params.GetInt("Survival Staged") != 0)
        QueueDeleteObjectID(id);
}

void DiscardPrepared(Raider@ r) {
    DeleteStaged(r.staged_actor);
    DeleteStaged(r.staged_weapon);
    r.staged_actor = r.staged_weapon = -1;
}

void DiscardForecast(int index) {
    Forecast@ f = forecasts[index];
    for (uint i = 0; i < f.fighters.length(); ++i) DiscardPrepared(f.fighters[i]);
    forecasts.removeAt(index);
}

Forecast@ Formation(int number, int wave, int team, int members) {
    int seed = LobbyGetModeSetting("survival_seed");
    for (int i = int(forecasts.length()) - 1; i >= 0; --i) {
        Forecast@ f = forecasts[i];
        if (f.team != team) continue;
        if (f.battery == number && f.wave == wave && f.members == members && f.seed == seed) return f;
        DiscardForecast(i);
    }
    Forecast@ f = Forecast();
    f.battery = number; f.wave = wave; f.team = team; f.members = members; f.seed = seed;
    f.fighters = PlanWave(seed, number, wave, members, team);
    forecasts.insertLast(f);
    return f;
}

// Build one body or weapon per director tick, ahead of combat on every peer.
// The graphics context, physics world and AngelScript instances belong to the
// game thread. Disabled objects have no AI or collision while waiting.
void PrepareNextWave(const array<PlayerState> &in seats) {
    bool next_battery = phase == resting;
    if (!next_battery && phase != preparing && !(phase == fighting && launched < wave_count)) return;
    array<int> present = RosterTeams(seats);
    for (int i = int(forecasts.length()) - 1; i >= 0; --i) {
        int team = forecasts[i].team;
        if (present.find(team) < 0 || (!next_battery && Members(seats, team, true).isEmpty()))
            DiscardForecast(i);
    }
    for (uint i = 0; i < present.length(); ++i) {
        int team = present[i];
        if (!next_battery && eliminated.find(team) >= 0) continue;
        int count = Members(seats, team, !next_battery).length();
        if (count == 0) continue;
        if (!next_battery) count = max(count, ScoreFor(team).peak_members);
        Forecast@ f = Formation(next_battery ? battery + 1 : battery, next_battery ? 1 : launched + 1, team, count);
        for (uint j = 0; j < f.fighters.length(); ++j) {
            Raider@ r = f.fighters[j];
            // The original deployment anchor is inside the playable world.
            // Map scripts can inspect objects even while their AI is disabled.
            vec3 parking = home;
            if (r.staged_actor < 0) {
                r.staged_actor = Online_PrepareSurvivalObject(actors[r.kind], parking);
                return;
            }
            if (r.weapon >= 0 && r.staged_weapon < 0) {
                r.staged_weapon = Online_PrepareSurvivalObject(weapons[r.weapon], parking);
                return;
            }
        }
    }
}

// Version-one saves only stored team and slot. Reconstruct those original
// opponents once; new snapshots store the purchases made at wave launch.
int LegacyEnemyTier() {
    if (battery < first_armed_battery) return 0;
    return min(12, 1 + (battery - first_armed_battery) / 3);
}

int LegacySpecies(int wave, int slot) {
    int tier = LegacyEnemyTier();
    int roll = (slot * 37 + wave * 17 + (battery % 100) * 29) % 100;
    if (tier >= 6 && roll >= 100 - min(16, 4 + (tier - 6) * 2)) return 5;
    if (tier >= 3 && roll >= 50 && roll < 50 + min(20, 6 + (tier - 3) * 2)) return 4;
    if (tier >= 1 && roll < min(28, 8 + tier * 2)) return 3;
    return (roll + slot) % 3;
}

int LegacyWeapon(int wave, int slot, int kind) {
    int tier = LegacyEnemyTier();
    if (tier == 0 || kind == 5) return -1;
    int roll = (slot * 47 + wave * 23 + (battery % 100) * 31) % 100;
    if (roll >= 8 + tier * 4) return -1;
    return (slot + wave + battery) % min(4, 1 + tier / 2);
}

bool Spawn(Raider@ r, int wave, const array<int> &in targets, const array<int> &in present) {
    if (targets.length() == 0) return false;
    int seed = wave + r.slot + r.team + r.spawn_attempt / 7;
    int target = targets[seed % targets.length()];
    vec3 anchor = ReadCharacterID(target).position;
    vec3 pos, rally;
    uint placement = WaveSeed(LobbyGetModeSetting("survival_seed"), battery, wave);
    placement = MixSeed(MixSeed(placement + uint(r.team)) + uint(r.slot)) + uint(r.spawn_attempt);
    if (!SpawnSpot(targets, anchor, SpawnRadii(battery), r.spawn_attempt % 7, pos, rally, placement)) return false;
    int kind = r.kind;
    int id = r.staged_actor;
    if (!Online_DeploySurvivalObject(id, pos)) id = CreateObject(actors[kind], true);
    if (!MovementObjectExists(id)) return false;
    r.staged_actor = -1;
    r.id = id;
    Object@ o = ReadObjectFromID(id);
    o.SetTranslation(pos);
    o.SetPlayer(false);
    ScriptParams@ p = o.GetScriptParams();
    p.SetInt("Survival Battery", battery);
    p.SetInt("Survival Team", r.team);
    p.SetInt("Survival Wave", wave);
    p.SetInt("Survival Slot", r.slot);
    SetTags(id, EnemyTags(r.team, present));
    int weapon = r.weapon;
    if (weapon >= 0) {
        int item = r.staged_weapon;
        if (!Online_DeploySurvivalObject(item, pos)) item = CreateObject(weapons[weapon], true);
        r.staged_weapon = -1;
        if (ObjectExists(item)) {
            Object@ w = ReadObjectFromID(item);
            w.SetTranslation(pos);
            w.GetScriptParams().SetInt("Survival Weapon", battery);
            o.AttachItem(w, _at_grip, false);
        }
    }
    Online_FreezeSurvivalEnemy(id, battery >= first_armed_battery);
    MovementObject@ mo = ReadCharacterID(id);
    mo.SetRotationFromFacing(normalize(vec3(anchor.x - pos.x, 0, anchor.z - pos.z)));
    // Stock and compatible custom minds can investigate the approach.
    // Sight and hearing decide if they acquire a fighter as a moving target.
    if (mo.HasFunction("vec3 GetInvestigateTargetPos()") && mo.HasFunction("void SetGoal(AIGoal)") &&
        mo.HasFunction("void SetSubGoal(AISubGoal)") && mo.HasVar("nav_target") && mo.HasVar("investigate_target_id"))
        mo.Execute("nav_target = vec3(" + rally.x + "," + rally.y + "," + rally.z + "); investigate_target_id = -1; " +
                   "SetGoal(_investigate); SetSubGoal(_investigate_urgent);");
    return true;
}

void Launch(const array<PlayerState> &in seats) {
    Wave@ w = Wave();
    w.number = ++launched;
    Online_ChallengeEncounterBegin(WaveKey(launched));
    Online_CaptureEnemyDifficulty(false);
    for (uint i = 0; i < teams.length(); ++i) {
        int team = teams[i];
        int n = Members(seats, team, true).length();
        if (n == 0 || eliminated.find(team) >= 0) continue;
        w.teams.insertLast(team);
        // Losing a teammate cannot make the last wave cheaper than the opener.
        TeamScore@ score = ScoreFor(team);
        if (launched == 1) score.last_contact = the_time;
        score.peak_members = max(score.peak_members, n);
        Forecast@ f = Formation(battery, launched, team, score.peak_members);
        for (uint j = 0; j < f.fighters.length(); ++j) w.remaining.insertLast(f.fighters[j]);
        f.fighters.resize(0);  // the live wave now owns these objects
    }
    while (!forecasts.isEmpty()) DiscardForecast(int(forecasts.length()) - 1);
    waves.insertLast(w);
    due = the_time + wave_interval;
}

void CleanupGear() {
    array<int>@ items = GetObjectIDsType(_item_object);
    for (uint i = 0; i < items.length(); ++i) {
        int id = items[i];
        ScriptParams@ params = ReadObjectFromID(id).GetScriptParams();
        if (!params.HasParam("Survival Recall Slot") && (params.HasParam("Survival Weapon") || params.HasParam("Campaign Weapon Owner")) &&
            ReadItemID(id).HeldByWhom() < 0 && ReadItemID(id).StuckInWhom() < 0) QueueDeleteObjectID(id);
    }
}

void DeployDownedSeats(const array<PlayerState> &in seats) {
    for (uint i = 0; i < seats.length(); ++i) {
        int id = seats[i].avatar_id;
        if (MovementObjectExists(id) && !Up(id)) {
            array<int> allies = Members(seats, seats[i].team, true);
            vec3 near = allies.length() == 0 ? home : ReadCharacterID(allies[0]).position;
            Online_PlaceAvatar(id, Online_SafeSpotNear(near, vec3(0, 0, 1), int(i + 1)), vec3(0, 0, 1));
        }
    }
}

void StartBattery(const array<PlayerState> &in seats) {
    teams = RosterTeams(seats);
    for (uint i = 0; i < teams.length(); ++i) ScoreFor(teams[i]);
    for (uint i = 0; i < scores.length(); ++i) scores[i].peak_members = 0;
    eliminated.resize(0);
    waves.resize(0);
    launched = 0;
    wave_count = WavesForBattery(battery);
    CleanupGear();
    winning_slots.resize(0);
    phase = preparing;
    awaiting_deployment = !HasSurvivor(seats);
    DeployDownedSeats(seats);
    due = the_time + deployment_seconds;
}

JSONValue Numbers(const array<int> &in nums) {
    JSONValue value(JSONarrayValue);
    for (uint i = 0; i < nums.length(); ++i) value.append(JSONValue(nums[i]));
    return value;
}
array<int> ReadNumbers(JSONValue value) {
    array<int> nums;
    for (uint i = 0; i < value.size(); ++i) nums.insertLast(value[int(i)].asInt());
    return nums;
}

void Publish() {
    JSON json;
    JSONValue s;
    s["map"] = JSONValue(GetCurrLevelRelPath());
    s["home"] = JSONValue(JSONarrayValue);
    s["home"].append(JSONValue(home.x)); s["home"].append(JSONValue(home.y)); s["home"].append(JSONValue(home.z));
    s["phase"] = JSONValue(int(phase)); s["battery"] = JSONValue(battery); s["launched"] = JSONValue(launched);
    s["wait"] = JSONValue(max(0, int(ceil(due - the_time))));
    s["teams"] = Numbers(teams); s["eliminated"] = Numbers(eliminated);
    if (awaiting_deployment) s["deploying"] = JSONValue(true);
    if (phase == victory || phase == resting) s["winners"] = Numbers(winning_slots);
    s["searching"] = JSONValue(JSONarrayValue);
    s["scores"] = JSONValue(JSONarrayValue);
    for (uint i = 0; i < scores.length(); ++i) {
        JSONValue row;
        row["team"] = JSONValue(scores[i].team);
        row["waves"] = JSONValue(scores[i].waves);
        row["kills"] = JSONValue(scores[i].kills);
        if (phase == fighting && the_time - scores[i].last_contact >= 45.0f) s["searching"].append(JSONValue(scores[i].team));
        row["idle_seconds"] = JSONValue(max(0.0f, the_time - scores[i].last_contact));
        row["peak_members"] = JSONValue(scores[i].peak_members);
        s["scores"].append(row);
    }
    s["waves"] = JSONValue(JSONarrayValue);
    int enemies = 0;
    for (uint i = 0; i < waves.length(); ++i) {
        Wave@ w = waves[i];
        JSONValue row;
        row["number"] = JSONValue(w.number); row["teams"] = Numbers(w.teams);
        row["remaining"] = JSONValue(JSONarrayValue);
        for (uint j = 0; j < w.remaining.length(); ++j) {
            JSONValue pair(JSONarrayValue);
            pair.append(JSONValue(w.remaining[j].team)); pair.append(JSONValue(w.remaining[j].slot));
            pair.append(JSONValue(w.remaining[j].kind)); pair.append(JSONValue(w.remaining[j].weapon));
            row["remaining"].append(pair);
            ++enemies;
        }
        s["waves"].append(row);
    }
    s["enemies"] = JSONValue(enemies); s["wave_count"] = JSONValue(wave_count);
    json.getRoot() = s;
    SessionSetModData(mod_id, json.writeString());
    next_publish = the_time + 3.0f;
}

// Saves recreate only the remaining formation slots. A promoted guest already
// owns a copy of the surviving fighters; adopt those actors by their wave tags.
bool Restore(JSONValue s, const array<PlayerState> &in seats) {
    if (!s["map"].isString() || s["map"].asString() != GetCurrLevelRelPath() ||
        !s["battery"].isInt() || s["battery"].asInt() < 1 || s["battery"].asInt() > 1000000 ||
        !s["phase"].isInt() || s["phase"].asInt() < preparing || s["phase"].asInt() > results ||
        !s["waves"].isArray() || s["waves"].size() > 7) return false;
    battery = s["battery"].asInt(); launched = min(7, max(0, s["launched"].asInt()));
    // A pre-rebalance save may already be fighting a longer battery.
    wave_count = WavesForBattery(battery);
    if (launched > 0) {
        if (s["wave_count"].isInt()) wave_count = s["wave_count"].asInt();
        wave_count = min(7, max(launched, wave_count));
    }
    if (s["home"].isArray() && s["home"].size() == 3)
        home = vec3(s["home"][0].asFloat(), s["home"][1].asFloat(), s["home"][2].asFloat());
    phase = Phase(s["phase"].asInt());
    int max_wait = phase == resting ? int(battery_break) + max(0, ProgressionSurvivalShopSeconds()) : int(wave_interval);
    due = the_time + min(max_wait, max(0, s["wait"].asInt()));
    teams = ReadNumbers(s["teams"]); eliminated = ReadNumbers(s["eliminated"]);
    winning_slots = ReadNumbers(s["winners"]);
    if ((phase == victory || phase == resting) && !s["winners"].isArray()) {
        // Older intermissions only saved team IDs. Include surviving avatars
        // even if the host has already reassigned their slot to a new team.
        array<int> winners = Winners(seats, teams);
        for (uint i = 0; i < seats.length(); ++i)
            if (Up(seats[i].avatar_id) || winners.find(seats[i].avatar_id) >= 0)
                winning_slots.insertLast(int(seats[i].id));
    }
    awaiting_deployment = phase == preparing && s["deploying"].isBool() && s["deploying"].asBool();
    scores.resize(0);
    if (s["scores"].isArray()) {
        for (uint i = 0; i < s["scores"].size(); ++i) {
            JSONValue row = s["scores"][int(i)];
            int team = row["team"].asInt();
            if (team <= 0 || team > 4096) continue;
            TeamScore@ score = ScoreFor(team);
            score.last_contact = the_time - min(45.0f, max(0.0f, row["idle_seconds"].asFloat()));
            score.waves = max(0, row["waves"].asInt());
            score.kills = max(0, row["kills"].asInt());
            score.peak_members = max(0, row["peak_members"].asInt());
        }
    }
    for (uint i = 0; i < teams.length(); ++i) {
        TeamScore@ score = ScoreFor(teams[i]);
        if (score.peak_members == 0 && launched > 0)
            score.peak_members = int(Members(seats, teams[i], false).length());
    }
    waves.resize(0);
    for (uint i = 0; i < s["waves"].size(); ++i) {
        JSONValue row = s["waves"][int(i)];
        Wave@ w = Wave(); w.number = row["number"].asInt(); w.teams = ReadNumbers(row["teams"]);
        for (uint j = 0; j < row["remaining"].size(); ++j) {
            JSONValue pair = row["remaining"][int(j)];
            Raider@ r = Raider(); r.team = pair[0].asInt(); r.slot = pair[1].asInt();
            if (pair.size() >= 4 && pair[2].isInt() && pair[3].isInt() &&
                pair[2].asInt() >= 0 && pair[2].asInt() < int(actors.length()) &&
                pair[3].asInt() >= -1 && pair[3].asInt() < int(weapons.length())) {
                r.kind = pair[2].asInt(); r.weapon = pair[3].asInt();
            } else {
                r.kind = LegacySpecies(w.number, r.slot);
                r.weapon = LegacyWeapon(w.number, r.slot, r.kind);
            }
            w.remaining.insertLast(r);
        }
        waves.insertLast(w);
    }
    for (int i = 0; i < GetNumCharacters(); ++i) {
        MovementObject@ mo = ReadCharacter(i);
        ScriptParams@ p = ReadObjectFromID(mo.GetID()).GetScriptParams();
        if (p.HasParam("Survival Staged") && p.GetInt("Survival Staged") != 0) {
            QueueDeleteObjectID(mo.GetID());
            continue;
        }
        if (!p.HasParam("Survival Slot")) continue;
        bool adopted = false;
        bool current = Up(mo.GetID()) && p.HasParam("Survival Battery") && p.GetInt("Survival Battery") == battery;
        for (uint j = 0; current && j < waves.length(); ++j) {
            if (waves[j].number != p.GetInt("Survival Wave")) continue;
            for (uint k = 0; k < waves[j].remaining.length(); ++k) {
                Raider@ r = waves[j].remaining[k];
                if (r.team == p.GetInt("Survival Team") && r.slot == p.GetInt("Survival Slot") && r.id < 0) {
                    r.id = mo.GetID(); adopted = true; break;
                }
            }
        }
        if (!adopted) QueueDeleteObjectID(mo.GetID());
    }
    array<int>@ items = GetObjectIDsType(_item_object);
    for (uint i = 0; i < items.length(); ++i) {
        ScriptParams@ p = ReadObjectFromID(items[i]).GetScriptParams();
        if (p.HasParam("Survival Staged") && p.GetInt("Survival Staged") != 0) QueueDeleteObjectID(items[i]);
    }
    return true;
}

void BeginDefeat() {
    while (!forecasts.isEmpty()) DiscardForecast(int(forecasts.length()) - 1);
    Online_ChallengeEncounterWipe(WaveKey(waves.length() == 0 ? max(1, launched) : waves[0].number));
    eliminated = teams;
    // Hold the final living opponents in the scene during the death camera.
    // Pending formation slots never entered the fight.
    for (uint i = 0; i < waves.length(); ++i) {
        Wave@ w = waves[i];
        for (int j = int(w.remaining.length()) - 1; j >= 0; --j) {
            Raider@ r = w.remaining[j];
            if (Up(r.id)) continue;
            DiscardPrepared(r);
            if (MovementObjectExists(r.id) && ReadCharacterID(r.id).GetIntVar("knocked_out") != 0)
                ++ScoreFor(r.team).kills;
            w.remaining.removeAt(j);
        }
    }
    phase = defeated; due = the_time + finish_camera_time;
    Publish();
    SessionFinishSurvivalRun();
}

bool HasSurvivor(const array<PlayerState> &in seats) {
    for (uint i = 0; i < seats.length(); ++i) {
        if (!Up(seats[i].avatar_id)) continue;
        if (phase == victory || phase == resting) {
            if (winning_slots.find(int(seats[i].id)) >= 0) return true;
        } else if (teams.find(seats[i].team) >= 0 && eliminated.find(seats[i].team) < 0) {
            return true;
        }
    }
    return false;
}

void Fight(const array<PlayerState> &in seats) {
    bool changed = false;
    array<int> present = RosterTeams(seats);
    // Newly joined teams enter with the next wave. Keep eliminated teams in
    // the original roster so switching seats cannot re-enter this battery.
    if (launched < wave_count) for (uint i = 0; i < present.length(); ++i)
        if (teams.find(present[i]) < 0 && Members(seats, present[i], true).length() > 0) {
            teams.insertLast(present[i]); ScoreFor(present[i]); changed = true;
        }
    for (uint i = 0; i < teams.length(); ++i)
        if (eliminated.find(teams[i]) < 0 && Members(seats, teams[i], true).length() == 0) {
            eliminated.insertLast(teams[i]); changed = true;
        }

    if (eliminated.length() == teams.length()) {
        BeginDefeat();
        return;
    }
    int spawn_budget = 3;
    bool built_actor = false;  // a short countdown or a new seat can leave some bodies unprepared
    for (int i = int(waves.length()) - 1; i >= 0; --i) {
        Wave@ w = waves[i];
        for (int j = int(w.remaining.length()) - 1; j >= 0; --j) {
            Raider@ r = w.remaining[j];
            if (eliminated.find(r.team) >= 0) {
                DiscardPrepared(r);
                if (MovementObjectExists(r.id)) QueueDeleteObjectID(r.id);
            } else if (r.id < 0) {
                bool cold = r.staged_actor < 0;
                if (spawn_budget > 0 && (!cold || !built_actor) && the_time >= r.retry_at) {
                    --spawn_budget;
                    if (!Spawn(r, w.number, Members(seats, r.team, true), present)) {
                        ++r.spawn_attempt;
                        r.retry_at = the_time + 0.6f;
                    } else if (cold) built_actor = true;
                }
                continue;
            } else if (Up(r.id)) {
                MovementObject@ foe = ReadCharacterID(r.id);
                if (foe.HasVar("last_damaged_time") && foe.HasVar("time") &&
                    foe.GetFloatVar("time") - foe.GetFloatVar("last_damaged_time") < 0.5f &&
                    foe.HasVar("attacked_by_id") && Members(seats, r.team, false).find(foe.GetIntVar("attacked_by_id")) >= 0)
                    ScoreFor(r.team).last_contact = the_time;
                if (foe.HasVar("state") && foe.GetIntVar("state") == 2 && foe.HasVar("target_id") &&
                    Members(seats, r.team, true).find(foe.GetIntVar("target_id")) >= 0)
                    ScoreFor(r.team).last_contact = the_time;
                SetTags(r.id, EnemyTags(r.team, present));
                continue;
            } else if (MovementObjectExists(r.id)) {
                corpses.insertLast(r.id); remove_at.insertLast(the_time + 4.0f);
            }
            // Both sides can fall in the same tick. Count the opponent even
            // when its team has just been eliminated and its cohort despawns.
            if (MovementObjectExists(r.id) && ReadCharacterID(r.id).GetIntVar("knocked_out") != 0) {
                TeamScore@ score = ScoreFor(r.team);
                ++score.kills;
                score.last_contact = the_time;
            }
            w.remaining.removeAt(j); changed = true;
            if (eliminated.find(r.team) < 0) {
                bool team_clear = true;
                for (uint k = 0; k < w.remaining.length(); ++k)
                    if (w.remaining[k].team == r.team) { team_clear = false; break; }
                if (team_clear) {
                    ++ScoreFor(r.team).waves;
                    ProgressionSurvivalClear(battery, w.number, Members(seats, r.team, false));
                }
            }
        }
        if (w.remaining.length() == 0) {
            array<int> winners = Winners(seats, w.teams);
            if (winners.length() > 0) {
                string key = WaveKey(w.number);
                Online_ChallengeEncounterWin(key);
            }
            waves.removeAt(i); changed = true;
        }
    }
    if (launched == wave_count && waves.length() == 0) {
        Online_ChallengeCompletion(BatteryKey());
        array<int> survivors = Winners(seats, teams);
        for (uint i = 0; i < seats.length(); ++i)
            if (survivors.find(seats[i].avatar_id) >= 0) winning_slots.insertLast(int(seats[i].id));
        phase = victory; due = the_time + finish_camera_time;
        level.SendMessage("ohl survival battery_complete");
        changed = true;
    } else if (launched < wave_count && the_time >= due) {
        Launch(seats); changed = true;
    }
    if (changed || the_time >= next_publish) Publish();
    if (phase == victory) SessionAutosave();
}

void Update(int is_paused) {
    if (Online_IsHosting() && phase == results && score_text != "") {
        array<MousePress>@ presses = GetRawMouseInputs();
        int newest = presses.length() == 0 ? -1 : int(presses[presses.length() - 1].s_id);
        bool clicked = last_result_press != -2 && newest >= 0 && last_result_press != newest;
        last_result_press = newest;
        int input = LobbyGetHostInputID();
        if (input >= 0 && is_paused == 0 && !PlayerMenuHasFocus(input)) {
            bool keyboard = GetLocalPlayerIndexForInputID(input) == GetLocalKeyboardPlayerIndex();
            if (keyboard && clicked) {
                MouseButton button = presses[presses.length() - 1].button;
                if (button == MouseButton::LEFT) { Online_RetrySurvivalRun(); return; }
                if (button == MouseButton::RIGHT) { Online_EndSurvivalRun(); return; }
            } else if (!keyboard) {
                if (GetInputPressed(input, "menu_return")) { Online_RetrySurvivalRun(); return; }
                if (GetInputPressed(input, "menu_cancel")) { Online_EndSurvivalRun(); return; }
            }
        }
    }
    if (is_paused != 0 || the_time < next_tick) return;
    next_tick = the_time + 0.2f;
    if (the_time >= next_hud) {
        next_hud = the_time + 0.5f;
        UpdateStatusText();
    }
    UpdateNeutralMarkers();
    if (!Online_IsHosting()) { host_ready = false; return; }

    array<PlayerState>@ seats = GetPlayerStates();
    if (!host_ready) {
        if (seats.length() == 0 || SessionResumingGameplay()) return;
        for (uint i = 0; i < seats.length(); ++i) if (!MovementObjectExists(seats[i].avatar_id)) return;
        host_ready = true;
        JSON json; json.parseString(SessionGetModData(mod_id));
        if (!Restore(json.getRoot(), seats)) {
            ProgressionArenaLoadouts();
            StartBattery(seats); Publish();
        } else if (awaiting_deployment) {
            DeployDownedSeats(seats);
        } else if (phase == defeated || phase == results) {
            SessionFinishSurvivalRun();
        }
    }
    for (int i = int(corpses.length()) - 1; i >= 0; --i) {
        if (the_time < remove_at[i]) continue;
        if (MovementObjectExists(corpses[i])) QueueDeleteObjectID(corpses[i]);
        corpses.removeAt(i); remove_at.removeAt(i);
    }
    if (the_time + 0.2f < due) PrepareNextWave(seats);
    if (phase == fighting) { Fight(seats); CountdownTick(); return; }
    if (phase == preparing || phase == victory || phase == resting) {
        if (HasSurvivor(seats)) awaiting_deployment = false;
        else if (!awaiting_deployment || the_time >= due) {
            BeginDefeat();
            return;
        }
    }
    if (phase == resting && ProgressionShopOpen() && ProgressionSurvivalShopSeconds() < 0) {
        due = the_time + battery_break;
        last_countdown_second = 0;
        return;
    }
    if (the_time < due) {
        CountdownTick();
        if (the_time >= next_publish) Publish();
        return;
    }
    if (phase == preparing) {
        if (ProgressionShopOpen()) ProgressionCloseSurvivalUpgrades();
        phase = fighting;
        Fight(seats);
    } else if (phase == victory) {
        phase = resting; due = the_time + battery_break;
        array<int> winners;
        for (uint i = 0; i < seats.length(); ++i)
            if (winning_slots.find(int(seats[i].id)) >= 0 && MovementObjectExists(seats[i].avatar_id))
                winners.insertLast(seats[i].avatar_id);
        ProgressionSurvivalClear(battery, 0, winners);
        if (ProgressionBeginShop(false)) {
            Online_RecallSurvivalWeapons();
            due += max(0, ProgressionSurvivalShopSeconds());
        }
        Publish(); SessionAutosave();
    } else if (phase == resting) {
        ++battery; StartBattery(seats); Publish();
    } else if (phase == defeated) {
        for (uint i = 0; i < waves.length(); ++i)
            for (uint j = 0; j < waves[i].remaining.length(); ++j) {
                int id = waves[i].remaining[j].id;
                if (MovementObjectExists(id)) QueueDeleteObjectID(id);
            }
        waves.resize(0);
        phase = results; due = the_time;
        array<MousePress>@ presses = GetRawMouseInputs();
        last_result_press = presses.length() == 0 ? -1 : int(presses[presses.length() - 1].s_id);
        Publish();
    }
}

void ReceiveMessage(string msg) {
    if (msg == "ohl survival battery_complete") PlaySoundGroup("Data/Sounds/versus/fight_win2.xml");
    if (msg == "ohl survival countdown 3" || msg == "ohl survival countdown 2" || msg == "ohl survival countdown 1") {
        int sound = PlaySound("Data/Sounds/lugaru/consolesuccess.ogg");
        SetSoundGain(sound, 0.4f);
        SetSoundPitch(sound, 1.75f);
    }
}

void UpdateScoreboard() {
    string summary = "Battery " + hud_state["battery"].asInt() + "    Wave " + hud_state["launched"].asInt() +
                     "/" + hud_state["wave_count"].asInt();
    JSONValue rows = hud_state["scores"];
    int count = int(rows.size());
    array<int> visible;
    if (count > 8) {
        // Each machine shows the teams of its couch players first.
        array<LobbyPlayer>@ roster = LobbyPlayers();
        for (uint i = 0; i < roster.length(); ++i) {
            if (!roster[i].local || roster[i].bot) continue;
            for (int j = 0; j < count; ++j) {
                if (rows[j]["team"].asInt() == roster[i].team && visible.find(j) < 0)
                    visible.insertLast(j);
            }
        }
    }
    // The host can leave the scoreboard open indefinitely.
    if (visible.length() > 8) {
        int pages = (int(visible.length()) + 7) / 8;
        int page = int(max(0.0f, the_time - hud_due) / scoreboard_page_seconds) % pages;
        array<int> local = visible;
        visible.resize(0);
        for (int i = page * 8; i < min(int(local.length()), page * 8 + 8); ++i) visible.insertLast(local[i]);
    }
    for (int i = 0; i < count && visible.length() < 8; ++i)
        if (visible.find(i) < 0) visible.insertLast(i);
    string next = "Game over\n" + summary;
    for (uint i = 0; i < visible.length(); ++i) {
        JSONValue row = rows[visible[i]];
        next += "\nTeam " + row["team"].asInt() + "    Waves " + row["waves"].asInt() +
                "    Enemies defeated " + row["kills"].asInt();
    }
    int others = count - int(visible.length());
    if (others > 0) next += "\n+" + others + " teams";
    string prompt = "Waiting for the host";
    if (Online_IsHosting()) {
        int input = LobbyGetHostInputID();
        prompt = input >= 0 && GetLocalPlayerIndexForInputID(input) == GetLocalKeyboardPlayerIndex() ?
                 "Left click to retry   /   Right click for lobby" : "Confirm to retry   /   Cancel for lobby";
    }
    next += "\n" + prompt;
    if (score_text == next) return;
    score_text = next;
    TextCanvasTexture@ canvas = level.GetTextElement(score_id);
    canvas.ClearTextCanvas();
    TextStyle style;
    style.font_face_id = GetFontFaceID("Data/Fonts/Lato-Regular.ttf", 36);
    canvas.SetPenColor(255,255,255,255); canvas.SetPenPosition(vec2(20,44));
    canvas.AddText("Game over", style, UINT32MAX);
    style.font_face_id = GetFontFaceID("Data/Fonts/Lato-Regular.ttf", 24);
    canvas.SetPenPosition(vec2(20,80)); canvas.AddText(summary, style, UINT32MAX);
    int y = 122;
    canvas.SetPenColor(205,205,205,255);
    canvas.SetPenPosition(vec2(20,y)); canvas.AddText("Team", style, UINT32MAX);
    canvas.SetPenPosition(vec2(148,y)); canvas.AddText("Waves cleared", style, UINT32MAX);
    canvas.SetPenPosition(vec2(320,y)); canvas.AddText("Enemies defeated", style, UINT32MAX);
    canvas.SetPenColor(255,255,255,255);
    for (uint i = 0; i < visible.length(); ++i) {
        JSONValue row = rows[visible[i]];
        y += 30;
        canvas.SetPenPosition(vec2(20,y)); canvas.AddText("" + row["team"].asInt(), style, UINT32MAX);
        canvas.SetPenPosition(vec2(148,y)); canvas.AddText("" + row["waves"].asInt(), style, UINT32MAX);
        canvas.SetPenPosition(vec2(320,y)); canvas.AddText("" + row["kills"].asInt(), style, UINT32MAX);
    }
    if (others > 0) {
        y += 30;
        canvas.SetPenPosition(vec2(20,y)); canvas.AddText("+" + others + " teams", style, UINT32MAX);
    }
    y += 40;
    style.font_face_id = GetFontFaceID("Data/Fonts/Lato-Regular.ttf", 22);
    canvas.SetPenPosition(vec2(20,y)); canvas.AddText(prompt, style, UINT32MAX);
    score_height = y + 20;
    canvas.UploadTextCanvasToTexture();
}

void UpdateStatusText() {
    string snapshot = SessionGetModData(mod_id);
    if (hud_snapshot != snapshot) {
        JSON json;
        if (!json.parseString(snapshot)) return;
        hud_state = json.getRoot();
        hud_snapshot = snapshot;
        hud_due = the_time + hud_state["wait"].asInt();
    }
    if (!hud_state["battery"].isInt()) return;
    int secs = max(0, int(ceil((Online_IsHosting() ? due : hud_due) - the_time)));
    int stage = hud_state["phase"].asInt();
    level.showing_results = stage == results;
    string line = "Battery " + hud_state["battery"].asInt();
    if (stage == preparing) line += "   Starts in " + secs + "s";
    if (stage == fighting) {
        int wave = hud_state["launched"].asInt(), total = hud_state["wave_count"].asInt();
        line += "   Wave " + wave + "/" + total + "\nEnemies " + hud_state["enemies"].asInt();
        if (wave < total) line += "   Next wave in " + secs + "s";
    }
    if (stage == resting) line += ProgressionShopOpen() && ProgressionSurvivalShopSeconds() < 0 ?
        "   Choose upgrades" : "   Next battery in " + (secs + deployment_seconds) + "s";
    if (stage == victory) line += " complete";
    if (stage == defeated) line += "   Team eliminated";
    if (stage == results) {
        UpdateScoreboard();
        return;
    }
    if (line != status_text) {
        status_text = line;
        TextCanvasTexture@ canvas = level.GetTextElement(text_id);
        canvas.ClearTextCanvas();
        TextStyle style; style.font_face_id = GetFontFaceID("Data/Fonts/Lato-Regular.ttf", 26);
        canvas.SetPenColor(255,255,255,255);
        array<string>@ rows = line.split("\n");
        status_width = 0;
        status_height = 14.0f + 30.0f * rows.length();
        for (uint i = 0; i < rows.length(); ++i) {
            TextMetrics metrics;
            canvas.GetTextMetrics(rows[i], style, metrics, UINT32MAX);
            status_width = max(status_width, ceil(metrics.advance_x / 64.0f) + 20.0f);
            canvas.SetPenPosition(vec2(10, 30 + 30 * i));
            canvas.AddText(rows[i], style, UINT32MAX);
        }
        canvas.UploadTextCanvasToTexture();
    }
}

void DrawGUI() {
    if (score_text != "") {
        float scale = min(1.0f, min(GetScreenWidth() * 0.88f / 512.0f, GetScreenHeight() * 0.85f / score_height));
        float left = (GetScreenWidth() - 512.0f * scale) * 0.5f;
        float top = (GetScreenHeight() + score_height * scale) * 0.5f;
        HUDImage@ panel = hud.AddImage();
        panel.SetImageFromPath("Data/Textures/ui/whiteblock.tga");
        panel.color = vec4(0.04f, 0.04f, 0.05f, 0.88f);
        panel.scale = vec3(512.0f * scale / panel.GetWidth(), score_height * scale / panel.GetHeight(), 1);
        panel.position = vec3(left, top - score_height * scale, 3);
        HUDImage@ score = hud.AddImage();
        score.SetImageFromText(level.GetTextElement(score_id));
        score.scale = vec3(scale, scale, 1);
        score.position = vec3(left, top - 512.0f * scale, 4);
        return;
    }
    DrawNeutralMarkers();
    if (text_id < 0 || status_text == "") return;
    vec2 placement = ProgressionLayoutArenaScore(20, 18, status_width, status_height);
    float top = GetScreenHeight() - 18;
    HUDImage@ panel = hud.AddImage();
    panel.SetImageFromPath("Data/Textures/ui/whiteblock.tga");
    panel.color = vec4(0.04f, 0.04f, 0.05f, 0.82f);
    panel.position = vec3(placement.x, top - status_height * placement.y, 3);
    panel.scale = vec3(status_width * placement.y / panel.GetWidth(), status_height * placement.y / panel.GetHeight(), 1);
    HUDImage@ image = hud.AddImage();
    image.SetImageFromText(level.GetTextElement(text_id));
    image.scale = vec3(placement.y, placement.y, 1);
    image.position = vec3(placement.x, top - 80 * placement.y, 4);
}
