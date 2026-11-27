//-----------------------------------------------------------------------------
//           Name: online_arena.as
//    Script Type: Level
//    Description: Overgrowth Online: the arenas (Waterfall Cave, Magma Arena,
//                 Rock Cave, Stucco Courtyard), player against player. The
//                 engine runs it instead of multi_arena.as / arena_level.as
//                 when the level is played online (Level::Initialize).
//
//                 A match takes one of the
//                 level's battle setups (spots, weapons, items, intro); a
//                 weapon match is a fight to the death (one point), the others
//                 are two points; the last team standing takes the round.
//                 Battle templates supply teams, positions and equipment.
//                 The host may instead force its manual lobby teams.
//                 The crowd and the arena music.
//
//                 The host creates lobby bots. In automatic team mode this
//                 script also fills open battle positions with temporary AI.
//                 Humans and bots who join enter the next round together.
//                 The randomizer (config "oo_arena_random"): every round each
//                 fighter, players and bots, becomes a random species and look
//                 with random gear; when it is switched off the players get
//                 their own characters back.
//                 Everyone has knockout shields (kArenaShields; full again at
//                 each round start).
//
//                 The host runs the matches and tells the guests:
//                   ohl arena text <text>                    the announcement ("" none)
//                   ohl arena score <max> (#<char> <points>)...
//                   ohl arena fight                          "Fight!": everyone heals
//                   ohl arena over                           the round is over
//                   ohl arena become #<char> <path>|own      the randomizer: that player's machine switches it
//                   ohl arena disarm                         empty every character's hands and sheaths (old weapons go)
//                 and places every player where it runs (Online_PlaceAvatar).
//        License: Apache-2.0 (as the Wolfire scripts it draws from)
//-----------------------------------------------------------------------------

#include "threatcheck.as"
#include "challenge_arena_events.as"
#include "music_load.as"
#include "multi_arena/multi_arena_music.as"

MusicSegmentHandler msh;

// ---- the match (host) ----------------------------------------------------

enum ArenaPhase {
    kWaiting,    // fewer than two players
    kIntro,      // players placed, the announcements
    kFighting,
    kRoundOver,
    kMatchOver,
    kShoppingNextMatch = 6,  // saved match phases keep their numeric IDs
    kLegacyRollingNextRound
}

ArenaPhase phase = kWaiting;
float phase_time = 0.0f;   // the_time when the phase began
int intro_step = 0;

array<int> bots;           // host-simulated fighters, including match opponents
array<int> fill_bots;      // owned by this Arena only, never lobby seats
int match_size = 2;

// The randomizer: this round's (per roster place) character and gear
bool random_round = false;
bool random_last = false;    // the round before was random (off now: own characters back)
bool become_pending = false; // this round switches characters (sent as it starts)
bool place_pending = false;  // a switching round: the fighters go to their spots once switched
float arm_at = 0.5f;         // the round's weapons, this long after its start
bool clear_pending = false;  // the old weapons go once every machine emptied its characters' hands

const int kArenaShields = 5;  // aschar.as "Knockout Shield": hits that don't knock you out
float next_shields = 0.0f;
array<string> rnd_paths;     // character file, "own" (the player's own), "" unchanged
array<string> rnd_hand;      // weapon in hand, "" none
array<bool> rnd_knife;       // a knife in the sheath too
array<bool> rnd_wolf;

const array<array<string>> kRndCharacters = {
    {"turner", "pale_turner", "male_rabbit_1", "male_rabbit_2", "male_rabbit_3", "female_rabbit_1", "female_rabbit_2",
     "female_rabbit_3", "raider_rabbit", "guard", "guard2", "base_guard", "heavy", "heavy2", "commander", "civ",
     "pale_rabbit_civ", "ancient", "femaleancient", "bijou", "jairo", "donatas", "reaper", "ghost"},
    {"cat", "male_cat", "female_cat", "striped_cat", "fancy_striped_cat", "cobus", "drika", "ferdi"},
    {"dog_soldier", "lt_dog_big", "lt_dog_female", "lt_dog_male_1", "lt_dog_male_2"},
    {"rat", "female_rat", "hooded_rat"},
    {"wolf", "male_wolf"}
};
// How often each of those comes up (of 100): a wolf outclasses everyone, so rarely
const array<int> kRndWeights = {30, 23, 22, 22, 3};

int RandomSpecies(bool wolf_taken) {
    int total = 0;
    for(uint i = 0; i < kRndWeights.length(); ++i) {
        total += (wolf_taken && i == 4) ? 0 : kRndWeights[i];
    }
    int r = rand() % total;
    for(uint i = 0; i < kRndWeights.length(); ++i) {
        int w = (wolf_taken && i == 4) ? 0 : kRndWeights[i];
        if(r < w) {
            return int(i);
        }
        r -= w;
    }
    return 0;
}
array<int> roster;         // the players' (and bots') characters in this match
array<int> points;         // their points
int max_points = 2;
bool weapons_match = false;
string intro = "";
int round_number = 0;
int round_winner = -1;
array<int> round_winners;  // all members of the winning team, including fallen allies
bool resume_challenge_round = false;

array<vec3> spots;           // where players stand at a round start, in order
array<string> spot_weapons;  // what each spot's player holds ("" nothing)
array<int> spot_teams;       // template team for each position (0: extra marker)
array<int> template_weights; // original team sizes, scaled to this roster
array<int> fighter_teams;    // effective match teams; manual lobby choices stay intact
array<int> fighter_spots;    // each fighter's matching template position/gear
bool template_ffa = false;
string battle_name = "";
string last_battle_name = "";
int setup_points = 0;
array<string> item_paths;    // the setup's weapons on the ground
array<vec3> item_spots;
vec3 arena_center;
array<int> round_items;      // weapons made for this round
bool arm_pending = false;    // the round's weapons come once the old ones are gone everywhere

float next_broadcast = 0.0f;

// Spawn markers: arena_spawn (by LocName) or character_spawn (Stucco Courtyard)
array<int> marker_ids;
array<string> marker_names;
array<int> marker_types;     // character_spawn game_type, -1 none

// ---- shown (everyone) ----------------------------------------------------

string shown_text = "";
float text_visible = 0.0f;
int text_id = -1;
const float kTextInkTop = 256.0f - 48.0f;
float text_ink_height = 0.0f;
int score_id = -1;
array<int> score_ids;        // the scoreboard: characters here
array<int> score_points;
int score_max = 2;
bool score_dirty = true;
const int kScoreLineHeight = 40;
const int kMaxScoreRows = 8;

// The crowd: each machine from what it sees
float audience_excitement = 0.0f;
int audience_sound_handle = -1;
float crowd_cheer_amount = 0.0f;
float crowd_cheer_vel = 0.0f;

// ---- level script entry points --------------------------------------------

void Init(string str) {
    PlaySong("sub-arena-loop");
    QueueSegment("fight1-ver1");
    text_id = TextInit(512, 512);
    score_id = TextInit(512, 512);
    audience_sound_handle = PlaySoundLoop(level.GetPath("crowd_sound"), 0.0f);
    FindMarkers();

    if(Online_IsClient()) {
        return;  // the host's matches, the host's characters
    }

    // The levels have no character: the host's own, on the first marker
    // (players who join get theirs from the engine, beside it)
    if(marker_ids.length() > 0) {
        Object@ marker = ReadObjectFromID(marker_ids[0]);
        int id = CreateObject(level.GetPath("char_player"), true);
        Object@ obj = ReadObjectFromID(id);
        obj.SetTranslation(Online_SafeSpotNear(marker.GetTranslation(), vec3(0.0f, 0.0f, 1.0f), 0));
        vec4 r = marker.GetRotationVec4();
        obj.SetRotation(quaternion(r.x, r.y, r.z, r.a));
        obj.SetPlayer(true);
        Online_AnnounceObject(id, level.GetPath("char_player"));  // made while loading: guests get it now, before its team
        ScriptParams@ params = obj.GetScriptParams();
        params.AddIntCheckbox("No Save", true);
        params.SetString("Teams", DesiredTeamForAvatar(id));
    }
    SetPhase(kWaiting);
}

bool HasFocus() {
    return false;
}

void DrawGUI() {
    if(score_ids.length() > 0) {  // the points
        HUDImage @image = hud.AddImage();
        image.SetImageFromText(level.GetTextElement(score_id));
        float top = 20.0f;
        float left = 40.0f;
        float height = (1 + min(int(score_ids.length()), kMaxScoreRows)) * kScoreLineHeight + 12;
        vec2 placement = ProgressionLayoutArenaScore(left, top, 512.0f, height);
        float scale = placement.y;
        image.scale = vec3(scale, scale, 1.0f);
        image.position.x = placement.x;
        image.position.y = GetScreenHeight() - top - 512.0f * scale;
        image.position.z = 4;
        image.color = vec4(1, 1, 1, 0.9f);
    }
    if(text_visible > 0.0f) {
        // Measure from the first line's top so changing scale keeps the
        // original baseline and the position of wrapped announcements stable.
        float original_top = 500.0f - 512.0f + kTextInkTop;
        vec2 placement = ProgressionLayoutArenaAnnouncement(original_top, 512.0f, text_ink_height);
        float scale = placement.y;
        HUDImage @image = hud.AddImage();
        image.SetImageFromText(level.GetTextElement(text_id));
        image.scale = vec3(scale, scale, 1.0f);
        image.position.x = (GetScreenWidth() - 512.0f * scale) / 2.0f;
        image.position.y = GetScreenHeight() - placement.x - (512.0f - kTextInkTop) * scale;
        image.position.z = 4;
        image.color = vec4(1, 1, 1, text_visible);
    }
}

void Update() {
    msh.Update();
    UpdateCrowd();

    if(score_dirty) {
        score_dirty = false;
        DrawScoreboard();
    }

    if(shown_text != "") {
        text_visible = min(1.0f, text_visible + time_step * 2.0f);
    } else {
        text_visible = max(0.0f, text_visible - time_step * 2.0f);
    }

    if(the_time > next_shields) {
        next_shields = the_time + 1.0f;
        OwnShields();
    }

    if(Online_IsHosting()) {
        HostUpdate();
    }
}

// Knockout shields for the characters this machine runs (its player's; the
// host's also the bots)
void OwnShields() {
    for(int i = 0, num = GetNumCharacters(); i < num; ++i) {
        MovementObject@ char = ReadCharacter(i);
        if(!((char.controlled && !char.remote) || bots.find(char.GetID()) >= 0)) {
            continue;
        }
        Object@ obj = ReadObjectFromID(char.GetID());
        ScriptParams@ params = obj.GetScriptParams();
        if(!params.HasParam("Knockout Shield")) {
            params.AddInt("Knockout Shield", kArenaShields);
            obj.UpdateScriptParams();
        } else if(params.GetInt("Knockout Shield") != kArenaShields) {
            params.SetInt("Knockout Shield", kArenaShields);
            obj.UpdateScriptParams();
        }
    }
}

void ReceiveMessage(string msg) {
    TokenIterator token_iter;
    token_iter.Init();
    if(!token_iter.FindNextToken(msg)) {
        return;
    }
    string token = token_iter.GetToken(msg);

    if(token == "ohl" && Online_IsClient()) {
        if(!token_iter.FindNextToken(msg) || token_iter.GetToken(msg) != "arena" || !token_iter.FindNextToken(msg)) {
            return;
        }
        string what = token_iter.GetToken(msg);
        if(what == "text") {
            ShowLocal(msg.length() > 15 ? msg.substr(15) : "");  // "ohl arena text " + text
        } else if(what == "score") {
            ReceiveScore(msg);
        } else if(what == "fight") {
            HealOwn();
            msh.StartFight();
        } else if(what == "over") {
            msh.MatchEnded();
        } else if(what == "disarm") {
            DisarmAll();
        } else if(what == "become" && token_iter.FindNextToken(msg)) {
            // "become #<char> <path>": ours switches here (the engine tells the others)
            string ref = token_iter.GetToken(msg);
            int id = ref.length() > 1 ? atoi(ref.substr(1)) : -1;
            if(token_iter.FindNextToken(msg) && MovementObjectExists(id) && ReadCharacterID(id).controlled && !ReadCharacterID(id).remote) {
                Become(id, token_iter.GetToken(msg));
            }
        }
        return;
    }

    if(token == "dispose_level") {
        StopSound(audience_sound_handle);
    } else if(token == "character_died") {
        audience_excitement += 4.0f;
    } else if(token == "character_knocked_out") {
        audience_excitement += 3.0f;
    } else if(token == "character_start_flip" || token == "character_start_roll") {
        audience_excitement += 0.4f;
    } else if(token == "character_failed_flip") {
        audience_excitement += 1.0f;
    } else if(token == "item_hit") {
        audience_excitement += 1.5f;
    } else if(token == "knocked_over" || token == "character_thrown") {
        audience_excitement += 1.5f;
    } else if(token == "passive_blocked") {
        audience_excitement += 0.5f;
    } else if(token == "active_blocked" || token == "dodged" || token == "character_throw_escape") {
        audience_excitement += 0.7f;
    } else if(token == "character_attack_feint" || token == "character_attack_missed") {
        audience_excitement += 0.4f;
    } else if(token == "cut") {
        audience_excitement += 2.0f;
    }
}

// ---- host: the match -------------------------------------------------------

string ArenaSeatKey(int avatar) {
    array<PlayerState>@ states = GetPlayerStates();
    for(uint i = 0; i < states.length(); ++i)
        if(states[i].avatar_id == avatar) return "seat" + states[i].id;
    return "fill" + fill_bots.find(avatar);
}

JSONValue ArenaSavePos(vec3 position) {
    JSONValue value(JSONarrayValue);
    value.append(JSONValue(position.x)); value.append(JSONValue(position.y)); value.append(JSONValue(position.z));
    return value;
}

vec3 ArenaLoadPos(JSONValue value) {
    return vec3(value[0].asFloat(), value[1].asFloat(), value[2].asFloat());
}

// Save the selected battle itself: rerolling it on load changes points to win,
// team assignments and starting weapons even when the score is retained.
void PublishArenaState() {
    if(Online_IsClient() || round_number < 1 || roster.length() == 0 ||
       spots.length() == 0 || fighter_teams.length() != roster.length()) return;
    JSON save;
    JSONValue value;
    value["version"] = JSONValue(1); value["round"] = JSONValue(round_number);
    value["phase"] = JSONValue(int(phase)); value["max_points"] = JSONValue(max_points);
    value["match_size"] = JSONValue(match_size); value["weapons"] = JSONValue(weapons_match);
    value["ffa"] = JSONValue(template_ffa); value["battle"] = JSONValue(battle_name);
    value["intro"] = JSONValue(intro);
    value["challenge_match"] = JSONValue(challenge_arena_match_key);
    value["challenge_round"] = JSONValue(challenge_arena_round_key);
    value["challenge_serial"] = JSONValue(challenge_arena_round_serial);
    value["spots"] = JSONValue(JSONarrayValue); value["weights"] = JSONValue(JSONarrayValue);
    value["items"] = JSONValue(JSONarrayValue); value["fighters"] = JSONValue(JSONarrayValue);
    for(uint i = 0; i < spots.length(); ++i) {
        JSONValue row;
        row["pos"] = ArenaSavePos(spots[i]);
        row["weapon"] = JSONValue(i < spot_weapons.length() ? spot_weapons[i] : "");
        row["team"] = JSONValue(i < spot_teams.length() ? spot_teams[i] : 0);
        value["spots"].append(row);
    }
    for(uint i = 0; i < template_weights.length(); ++i) value["weights"].append(JSONValue(template_weights[i]));
    for(uint i = 0; i < item_paths.length(); ++i) {
        JSONValue row; row["path"] = JSONValue(item_paths[i]); row["pos"] = ArenaSavePos(item_spots[i]);
        value["items"].append(row);
    }
    for(uint i = 0; i < roster.length(); ++i) {
        JSONValue row;
        row["key"] = JSONValue(ArenaSeatKey(roster[i])); row["points"] = JSONValue(points[i]);
        row["team"] = JSONValue(fighter_teams[i]); row["spot"] = JSONValue(fighter_spots[i]);
        row["winner"] = JSONValue(round_winners.find(roster[i]) >= 0);
        value["fighters"].append(row);
    }
    save.getRoot() = value;
    SessionArenaState(save.writeString());
}

bool RestoreArenaMatch() {
    string state = SessionArenaRestore();
    if(state == "") return false;
    array<PlayerState>@ states = GetPlayerStates();
    array<int> seats = RosterSeats();
    if(seats.length() < states.length()) return true; // wait for native deployment
    JSON save; if(!save.parseString(state)) return false;
    JSONValue value = save.getRoot();
    max_points = value["max_points"].asInt(); match_size = value["match_size"].asInt();
    weapons_match = value["weapons"].asBool(); template_ffa = value["ffa"].asBool();
    battle_name = value["battle"].asString(); last_battle_name = battle_name;
    intro = value["intro"].asString(); setup_points = max_points;
    challenge_arena_match_key = value["challenge_match"].asString();
    challenge_arena_round_key = value["challenge_round"].asString();
    challenge_arena_round_serial = value["challenge_serial"].asInt();
    spots.resize(0); spot_weapons.resize(0); spot_teams.resize(0); template_weights.resize(0);
    item_paths.resize(0); item_spots.resize(0); arena_center = vec3(0);
    for(uint i = 0; i < value["spots"].size(); ++i) {
        JSONValue row = value["spots"][i]; spots.insertLast(ArenaLoadPos(row["pos"]));
        spot_weapons.insertLast(row["weapon"].asString()); spot_teams.insertLast(row["team"].asInt());
        arena_center += spots[i];
    }
    arena_center /= float(spots.length());
    for(uint i = 0; i < value["weights"].size(); ++i) template_weights.insertLast(value["weights"][i].asInt());
    for(uint i = 0; i < value["items"].size(); ++i) {
        item_paths.insertLast(value["items"][i]["path"].asString());
        item_spots.insertLast(ArenaLoadPos(value["items"][i]["pos"]));
    }
    FillOpenSpots(int(seats.length())); SyncBots();
    roster.resize(0); points.resize(0); fighter_teams.resize(0); fighter_spots.resize(0);
    round_winners.resize(0); round_winner = -1;
    array<int> fighters = Fighters();
    // Match by durable unit slot, never by this world's transient object ID.
    for(uint i = 0; i < value["fighters"].size(); ++i) {
        JSONValue row = value["fighters"][i];
        for(uint j = 0; j < fighters.length(); ++j) if(ArenaSeatKey(fighters[j]) == row["key"].asString()) {
            int avatar = fighters[j]; roster.insertLast(avatar); points.insertLast(row["points"].asInt());
            fighter_teams.insertLast(row["team"].asInt()); fighter_spots.insertLast(row["spot"].asInt());
            if(row["winner"].asBool()) { round_winners.insertLast(avatar); round_winner = avatar; }
        }
    }
    bool added = false;
    for(uint i = 0; i < fighters.length(); ++i) if(roster.find(fighters[i]) < 0) {
        roster.insertLast(fighters[i]); points.insertLast(0); added = true;
    }
    if(added) ConfigureMatchTeams();
    else LobbyAssignModeTeams(roster, fighter_teams);
    round_number = value["round"].asInt();
    ArenaPhase saved_phase = ArenaPhase(value["phase"].asInt());
    if(saved_phase == kIntro || saved_phase == kFighting || saved_phase == kWaiting) {
        resume_challenge_round = saved_phase == kFighting;
        --round_number;
        StartRound(true); // redeploy this round, retaining the saved score/setup
    } else {
        // An awarded win stays awarded. Continue its delay/shop boundary and
        // advance once, without calling challenge win/completion hooks again.
        phase = saved_phase; phase_time = the_time; intro_step = 0;
        SessionArenaRound(round_number);
        PlaceFighters(); SendScore(); PublishArenaState();
        Show(saved_phase == kShoppingNextMatch || saved_phase == kMatchOver ? "Match complete" : "Round complete");
    }
    return true;
}

void SetPhase(ArenaPhase p) {
    if(p != phase) {
        if(p == kFighting) {
            if(!resume_challenge_round) ChallengeArenaRoundBegin();
            resume_challenge_round = false;
        }
        else if(p == kRoundOver) {
            ChallengeArenaRoundWin(round_winner);
            ProgressionArenaRoundComplete(round_winners, roster);
        }
        else if(p == kMatchOver) {
            ChallengeArenaMatchComplete();
            ProgressionArenaWin(round_winners, true);
        }
    }
    phase = p;
    phase_time = the_time;
    intro_step = 0;
    PublishArenaState();
}

void HostUpdate() {
    if(round_number == 0 && RestoreArenaMatch()) return;
    if(ProgressionShopOpen()) return;
    float t = the_time - phase_time;
    DropGonePlayers();

    if(phase == kWaiting) {
        array<int> fighters = RosterSeats();
        if(LobbyGetForceTeams() ? TeamCount(fighters) >= 2 : fighters.length() > 0) {
            if(shown_text != "") {
                Show("");
            }
            if(t > 3.0f) {
                NewMatch();
            }
        } else {
            phase_time = the_time;
            string waiting = fighters.length() >= 2 ?
                "Waiting for another team" : "Waiting for another player";
            if(shown_text != waiting) {
                Show(waiting);
            }
        }
    } else if(TeamCount(roster) < 2) {
        // A team has nobody left in the match: wait for an opponent.
        roster.resize(0);
        points.resize(0);
        SendScore();
        SetPhase(kWaiting);
    } else if(phase == kIntro) {
        if(clear_pending && t > 0.3f) {
            clear_pending = false;
            ClearWeapons();
            if(become_pending) {
                become_pending = false;
                SendBecome();
            }
        }
        if(place_pending && t > 0.7f) {
            place_pending = false;
            PlaceFighters();
        }
        if(arm_pending && t > arm_at) {
            arm_pending = false;
            ArmPlayers();
            ProgressionArenaLoadouts();
        }
        if(round_number == 1 && intro_step == 0 && t > 2.0f) {
            Show(max_points == 1 ? "This is a fight to the death!" : max_points + " points to win!");
            intro_step = 1;
        }
        float fight_at = round_number == 1 ? 4.0f : 2.0f;
        if(t > fight_at && (AllUp() || t > fight_at + 3.0f)) {
            Show("Fight!");
            BotsFight();
            HealOwn();
            level.SendMessage("ohl arena fight");
            msh.StartFight();
            SetPhase(kFighting);
        }
    } else if(phase == kFighting) {
        if(t > 2.0f && shown_text == "Fight!") {
            Show("");
        }
        array<int> up;
        for(uint k = 0; k < roster.length(); ++k) {
            if(Up(roster[k])) {
                up.insertLast(roster[k]);
            }
        }
        if(TeamCount(up) <= 1) {
            round_winner = up.length() > 0 ? up[0] : -1;
            round_winners.resize(0);
            if(round_winner != -1) {
                string team = DesiredTeamForAvatar(round_winner);
                for(uint k = 0; k < roster.length(); ++k) {
                    if(DesiredTeamForAvatar(roster[k]) == team) {
                        ++points[k];
                        round_winners.insertLast(roster[k]);
                    }
                }
                Show(RoundWinnerName() + " wins the round!");
            } else {
                Show("Nobody wins the round!");
            }
            SendScore();
            level.SendMessage("ohl arena over");
            msh.MatchEnded();
            SetPhase(kRoundOver);
        }
    } else if(phase == kRoundOver) {
        if(t > 3.0f) {
            bool match_won = false;
            for(uint k = 0; k < round_winners.length(); ++k) {
                int at = roster.find(round_winners[k]);
                if(at >= 0 && points[at] >= max_points) {
                    match_won = true;
                }
            }
            if(match_won) {
                Show(RoundWinnerName() + " wins the match!");
                SetPhase(kMatchOver);
            } else {
                StartRound();
            }
        }
    } else if(phase == kMatchOver && t > 4.0f) {
        if(ProgressionBeginShop(false)) SetPhase(kShoppingNextMatch);
        else NewMatch();
    } else if(phase == kShoppingNextMatch) {
        NewMatch();
    } else if(phase == kLegacyRollingNextRound) {
        StartRound();
    }

    // Late joiners see where things stand
    if(the_time > next_broadcast) {
        next_broadcast = the_time + 2.0f;
        level.SendMessage("ohl arena text " + shown_text);
        SendScore();
    }
}

void NewMatch() {
    ChallengeArenaNewMatch();
    SetUpMatch();
    roster.resize(0);
    points.resize(0);
    round_number = 0;
    StartRound();
}

void StartRound(bool resume = false) {
    // A resumed round already has its attempt and unbanked XP from the save.
    if(!resume) ProgressionResetAttempt();
    ++round_number;
    round_winner = -1;
    round_winners.resize(0);

    array<int> seats = RosterSeats();
    for(uint k = 0; round_number > 1 && k < seats.length(); ++k) {
        if(roster.find(seats[k]) < 0) {
            // A late seat takes part at a round boundary. Pick a new match
            // before filling its open places so nobody inherits another score.
            round_number = 1;
            SetUpMatch();
            roster.resize(0);
            points.resize(0);
        }
    }
    FillOpenSpots(int(seats.length()));
    DropGonePlayers();

    // Everyone here plays (late joiners from the next round on), on their lobby teams.
    array<int> players = Fighters();
    for(uint k = 0; k < players.length(); ++k) {
        if(roster.find(players[k]) < 0) {
            roster.insertLast(players[k]);
            points.insertLast(0);
        }
    }
    if((round_number == 1 && !resume) || fighter_teams.length() != roster.length()) {
        ConfigureMatchTeams();
    }

    round_number = SessionArenaRound(round_number);
    for(uint i=0; i<roster.length(); ++i) points[i] = SessionArenaPoints(roster[i],points[i]);
    SessionAutosave();

    // Every weapon from before goes, in hands and on the ground (this round's
    // are made new). First every machine empties the hands and sheaths of the
    // characters it has (a script kept one deleted under it: "There is no item
    // object"), then they are deleted
    DisarmAll();
    level.SendMessage("ohl arena disarm");
    clear_pending = true;

    // A randomizer round switches everyone (with the deletes) and places them
    // after (a body switched where it stood came out stuck in walls and floors)
    PlanRandomizer();
    place_pending = become_pending;
    if(become_pending) {
        arm_at = 1.4f;
    } else {
        PlaceFighters();
        arm_at = 1.0f;
    }
    arm_pending = true;  // (ArmPlayers, after the old weapons are gone everywhere)

    SendScore();
    Show(round_number == 1 ? intro + " " + MatchDescription() : "Round " + round_number);
    SetPhase(kIntro);
}

// Every fighter to its spot, stood up (and the bots held until "Fight!")
void PlaceFighters() {
    for(uint k = 0; k < roster.length(); ++k) {
        int id = roster[k];
        vec3 pos, facing;
        SpotFor(k, pos, facing);
        Online_PlaceAvatar(id, pos, facing);
        if(bots.find(id) >= 0) {
            ReadCharacterID(id).ReceiveScriptMessage("set_combat_allowed false");  // (until "Fight!")
        }
    }
}

// The round's weapons. Half a second after the old ones were deleted: a hand
// still holding one turns a new weapon away (aschar.as HandleEditorAttachment),
// and the deletes reach every machine first (NotifyItemDetach empties the hand)
void ArmPlayers() {
    for(uint k = 0; k < roster.length(); ++k) {
        int id = roster[k];
        bool random = random_round && k < rnd_hand.length();
        int setup = k < fighter_spots.length() ? fighter_spots[k] : int(k);
        string weapon = random ? rnd_hand[k] : (spot_weapons.length() > 0 ? spot_weapons[setup % int(spot_weapons.length())] : "");
        if(!MovementObjectExists(id) || ReadCharacterID(id).GetIntVar("species") == 1 || (random && rnd_wolf[k])) {
            continue;  // (aschar.as _wolf: no hands)
        }
        if(weapon != "") {
            Arm(id, weapon, _at_grip);
        }
        if(random && rnd_knife[k]) {
            Arm(id, WeaponPath("knife"), _at_sheathe);
        }
    }

    for(uint k = 0; k < item_paths.length(); ++k) {
        int item_id = CreateObject(item_paths[k], true);
        Object@ item = ReadObjectFromID(item_id);
        item.SetTranslation(item_spots[k] + vec3(0.0f, 0.5f, 0.0f));
        item.GetScriptParams().AddIntCheckbox("No Save", true);
        round_items.insertLast(item_id);
    }
}

// Every character here lets go of what it holds and carries (every machine,
// before the host deletes the old weapons)
const string kDisarm = "for(int s = 0; s < 6; ++s) { int w = weapon_slots[s]; if(w != -1) { "
                       "if(ItemExists(w)) { this_mo.DetachItem(w); } NotifyItemDetach(w); } } ";

void DisarmAll() {
    for(int i = 0, num = GetNumCharacters(); i < num; ++i) {
        ReadCharacter(i).Execute(kDisarm);
    }
}

// The weapons the matches made (ours, and any "No Save" one: the level's own
// stay), wherever they are
void ClearWeapons() {
    array<int> @items = GetObjectIDsType(_item_object);
    for(uint k = 0; k < items.length(); ++k) {
        if(round_items.find(items[k]) >= 0 || ReadObjectFromID(items[k]).GetScriptParams().HasParam("No Save")) {
            QueueDeleteObjectID(items[k]);
        }
    }
    round_items.resize(0);
}

void Arm(int id, const string &in weapon, AttachmentType how) {
    int item_id = CreateObject(weapon, true);
    Object@ item = ReadObjectFromID(item_id);
    item.SetTranslation(ReadCharacterID(id).position);
    item.GetScriptParams().AddIntCheckbox("No Save", true);
    ReadObjectFromID(id).AttachItem(item, how, false);
    round_items.insertLast(item_id);
}

// ---- host: the randomizer ---------------------------------------------------

bool ArenaRandomizer() {
    return ConfigHasKey("oo_arena_random") && GetConfigValueBool("oo_arena_random");
}

// This round's characters and gear (on: random for everyone; switched off:
// the players their own characters back, the bots stay as they are)
void PlanRandomizer() {
    random_round = ArenaRandomizer();
    rnd_paths.resize(0);
    rnd_hand.resize(0);
    rnd_knife.resize(0);
    rnd_wolf.resize(0);
    array<string> weapons = {"", "", "knife", "knife", "sword", "sword", "big_sword", "spear", "spear"};
    bool wolf_taken = false;
    for(uint k = 0; k < roster.length(); ++k) {
        if(random_round) {
            int s = RandomSpecies(wolf_taken);  // (one wolf a round at most)
            wolf_taken = wolf_taken || s == 4;
            const array<string>@ kinds = kRndCharacters[s];
            rnd_paths.insertLast("Data/Characters/" + kinds[rand() % int(kinds.length())] + ".xml");
            rnd_wolf.insertLast(s == 4);
            string w = weapons[rand() % int(weapons.length())];
            rnd_hand.insertLast(w == "" ? "" : WeaponPath(w));
            rnd_knife.insertLast(rand() % 10 < 3);
        } else {
            rnd_paths.insertLast(random_last && bots.find(roster[k]) < 0 ? "own" : "");
            rnd_wolf.insertLast(false);
            rnd_hand.insertLast("");
            rnd_knife.insertLast(false);
        }
    }
    become_pending = random_round || random_last;
    random_last = random_round;
}

// Each machine switches its own characters: the host its own and the bots
void SendBecome() {
    for(uint k = 0; k < roster.length() && k < rnd_paths.length(); ++k) {
        int id = roster[k];
        if(rnd_paths[k] == "" || !MovementObjectExists(id)) {
            continue;
        }
        MovementObject@ char = ReadCharacterID(id);
        if(bots.find(id) >= 0 || (char.controlled && !char.remote)) {
            Become(id, rnd_paths[k]);
        } else {
            level.SendMessage("ohl arena become #" + id + " " + rnd_paths[k]);
        }
    }
}

// A character this machine runs becomes `path` ("own": its lobby appearance)
void Become(int id, const string &in path) {
    string p = path;
    if(p == "own") {
        // char_player is an Actor object file, not a Character definition.
        // Feeding it to SwitchCharacter can leave an invalid rig and hang
        // asset loading on every peer when the randomizer is switched off.
        p = "Data/Characters/turner.xml";
    }
    if(p == "" || !FileExists(p)) {
        return;
    }
    ReadCharacterID(id).Execute(kDisarm + "SwitchCharacter(\"" + p + "\"); FixDiscontinuity();");  // (SwitchCharacter detaches all but kept the sheaths named)
    if(path == "own") {
        Online_RestoreOwnCharacter();  // restore the lobby's team colours
    }
}

// Every ready seat gets exactly one fighter. Native owns creation/removal and
// the mapping from a persistent lobby seat to this level's actor.
array<int> RosterSeats() {
    array<int> ids;
    array<PlayerState>@ states = GetPlayerStates();
    for(uint k = 0; k < states.length(); ++k) {
        int id = states[k].avatar_id;
        if(id >= 0 && MovementObjectExists(id) && ids.find(id) < 0) {
            ids.insertLast(id);
        }
    }
    return ids;
}

array<int> Fighters() {
    array<int> ids = RosterSeats();
    for(uint i = 0; i < fill_bots.length(); ++i)
        if(MovementObjectExists(fill_bots[i])) ids.insertLast(fill_bots[i]);
    return ids;
}

// Players who left drop out; keep every avatar on its lobby team, including
// newly possessed characters and characters replaced by the randomizer.
void DropGonePlayers() {
    SyncBots();
    array<int> players = Fighters();
    for(int k = int(roster.length()) - 1; k >= 0; --k) {
        if(players.find(roster[k]) < 0) {
            roster.removeAt(k);
            points.removeAt(k);
            if(k < int(fighter_teams.length())) fighter_teams.removeAt(k);
            if(k < int(fighter_spots.length())) fighter_spots.removeAt(k);
            score_dirty = true;
        }
    }
    for(uint k = 0; k < players.length(); ++k) {
        OwnTeam(players[k]);
    }
}

void OwnTeam(int id) {
    Object@ obj = ReadObjectFromID(id);
    ScriptParams@ params = obj.GetScriptParams();
    string team = DesiredTeamForAvatar(id);
    if(params.GetString("Teams") != team) {
        params.SetString("Teams", team);
        obj.UpdateScriptParams();
    }
}

string DesiredTeamForAvatar(int id) {
    int assigned = LobbyGetAssignedTeam(id);
    if(assigned > 0) return "arena_team_" + assigned;
    array<PlayerState>@ states = GetPlayerStates();
    for(uint k = 0; k < states.length(); ++k) {
        if(states[k].avatar_id == id) {
            if(!LobbyGetForceTeams()) return "player" + id;
            if(states[k].team > 1) return "lobby_team_" + states[k].team;
            // Native Team1 preserves the map's friendly faction until the
            // match publishes its arena teams. Avoid fighting that assignment.
            ScriptParams@ params = ReadObjectFromID(id).GetScriptParams();
            return params.HasParam("Teams") ? params.GetString("Teams") : "lobby_team_1";
        }
    }
    return "player" + id;  // first spawn, before the engine possesses it
}

int TeamCount(const array<int> &in fighters) {
    array<string> teams;
    for(uint k = 0; k < fighters.length(); ++k) {
        string team = DesiredTeamForAvatar(fighters[k]);
        if(teams.find(team) < 0) {
            teams.insertLast(team);
        }
    }
    return int(teams.length());
}

string RoundWinnerName() {
    if(round_winners.length() > 1) {
        int team = LobbyGetAssignedTeam(round_winner);
        if(team > 0) return "Team " + team;
    }
    return PlayerName(round_winner);
}

bool Up(int id) {
    return MovementObjectExists(id) && ReadCharacterID(id).GetIntVar("knocked_out") == _awake;
}

bool AllUp() {
    for(uint k = 0; k < roster.length(); ++k) {
        if(!Up(roster[k])) {
            return false;
        }
    }
    return true;
}

string PlayerName(int id) {
    array<PlayerState>@ states = GetPlayerStates();
    for(uint k = 0; k < states.length(); ++k) {
        if(states[k].avatar_id == id && states[k].playername != "") {
            return states[k].playername;
        }
    }
    if(ObjectExists(id) && ReadObjectFromID(id).GetScriptParams().HasParam("Bot Name")) {
        return ReadObjectFromID(id).GetScriptParams().GetString("Bot Name");  // (the host's, on every machine)
    }
    return "Player";
}

void HealOwn() {
    int k = GetPlayerCharacterID();  // (an index)
    if(k != -1) {
        ReadCharacter(k).ReceiveScriptMessage("restore_health");
    }
}

// ---- host: bots -----------------------------------------------------------

// Track the host-simulated actors without creating or deleting roster seats.
// A late bot waits quietly until the next round, as a late human does.
void SyncBots() {
    bots = fill_bots;
    array<PlayerState>@ states = GetPlayerStates();
    for(uint k = 0; k < states.length(); ++k) {
        int id = states[k].avatar_id;
        if(states[k].is_bot && id >= 0 && MovementObjectExists(id) && bots.find(id) < 0) {
            bots.insertLast(id);
            if(!ReadObjectFromID(id).GetScriptParams().HasParam("Arena AI Initialized")) ConfigureBot(id);
        }
    }
}

void FillOpenSpots(int seats) {
    int wanted = LobbyGetForceTeams() || seats == 0 ? 0 : max(0, match_size - seats);
    for(int i = int(fill_bots.length()) - 1; i >= 0; --i)
        if(!MovementObjectExists(fill_bots[i])) fill_bots.removeAt(i);
    while(int(fill_bots.length()) > wanted) {
        QueueDeleteObjectID(fill_bots[fill_bots.length() - 1]);
        fill_bots.removeLast();
    }
    const array<string> paths = {"char_guard", "char_raider", "char_civ"};
    while(int(fill_bots.length()) < wanted) {
        string path = level.GetPath(paths[rand() % int(paths.length())]);
        int id = CreateObject(path, true);
        if(!MovementObjectExists(id)) break;
        Object@ object = ReadObjectFromID(id);
        object.SetPlayer(false);
        if(spots.length() > 0) object.SetTranslation(spots[fill_bots.length() % spots.length()]);
        object.GetScriptParams().AddIntCheckbox("No Save", true);
        object.GetScriptParams().AddString("Bot Name", "Arena fighter " + (fill_bots.length() + 1));
        ConfigureBot(id);
        fill_bots.insertLast(id);
    }
}

// Keep the arena's middling AI tuning, on the actor native assigned this seat.
void ConfigureBot(int id) {
    Object@ obj = ReadObjectFromID(id);
    ScriptParams@ params = obj.GetScriptParams();
    params.AddIntCheckbox("Arena AI Initialized", true);
    if(!params.HasParam("Knockout Shield")) params.AddInt("Knockout Shield", kArenaShields);
    float d = 0.5f;
    params.SetFloat("Character Scale", mix(RangedRandomFloat(0.9f, 1.0f), RangedRandomFloat(1.0f, 1.1f), d));
    params.SetFloat("Block Follow-up", mix(RangedRandomFloat(0.01f, 0.25f), RangedRandomFloat(0.75f, 1.0f), d));
    params.SetFloat("Block Skill", mix(RangedRandomFloat(0.01f, 0.25f), RangedRandomFloat(0.5f, 0.8f), d));
    params.SetFloat("Movement Speed", mix(RangedRandomFloat(0.8f, 1.0f), RangedRandomFloat(0.9f, 1.1f), d));
    params.SetFloat("Attack Speed", mix(RangedRandomFloat(0.8f, 1.0f), RangedRandomFloat(0.9f, 1.1f), d));
    float damage = mix(RangedRandomFloat(0.3f, 0.5f), RangedRandomFloat(0.9f, 1.1f), d);
    params.SetFloat("Attack Knockback", damage);
    params.SetFloat("Attack Damage", damage);
    params.SetFloat("Aggression", RangedRandomFloat(0.25f, 0.75f));
    params.SetFloat("Ground Aggression", d);
    params.SetFloat("Damage Resistance", mix(RangedRandomFloat(0.6f, 0.8f), RangedRandomFloat(0.9f, 1.1f), d));
    obj.UpdateScriptParams();
    ReadCharacterID(id).ReceiveScriptMessage("set_combat_allowed false");
}

// "Fight!": the bots may fight, and know everyone in the match
void BotsFight() {
    for(uint k = 0; k < bots.length(); ++k) {
        if(!MovementObjectExists(bots[k]) || roster.find(bots[k]) < 0) {
            continue;
        }
        MovementObject@ bot = ReadCharacterID(bots[k]);
        bot.ReceiveScriptMessage("set_combat_allowed true");
        for(uint j = 0; j < roster.length(); ++j) {
            if(roster[j] != bots[k] && MovementObjectExists(roster[j])) {
                bot.ReceiveScriptMessage("notice " + roster[j]);
            }
        }
    }
}

// ---- host: the match setup -------------------------------------------------

void FindMarkers() {
    marker_ids.resize(0);
    marker_names.resize(0);
    marker_types.resize(0);
    array<int> @ids = GetObjectIDs();
    for(uint i = 0; i < ids.length(); ++i) {
        ScriptParams@ params = ReadObjectFromID(ids[i]).GetScriptParams();
        if(!params.HasParam("Name")) {
            continue;
        }
        string name = params.GetString("Name");
        if(name == "arena_spawn" && params.HasParam("LocName") && params.GetString("LocName") != "") {
            marker_ids.insertLast(ids[i]);
            marker_names.insertLast(params.GetString("LocName"));
            marker_types.insertLast(-1);
        } else if(name == "character_spawn") {
            marker_ids.insertLast(ids[i]);
            marker_names.insertLast("");
            marker_types.insertLast(params.HasParam("game_type") ? atoi(params.GetString("game_type")) : -1);
        }
    }
}

string WeaponPath(const string &in name) {
    string w = name;
    if(w == "any") {
        array<string> any = {"knife", "big_sword", "sword", "spear"};
        w = any[rand() % 4];
    }
    if(w == "knife" || w == "big_sword" || w == "sword" || w == "spear") {
        return level.GetPath("weap_" + w);
    }
    return "";
}

vec3 MarkerPos(int at) {
    return ReadObjectFromID(marker_ids[at]).GetTranslation();
}

// Spots, weapons and items from one of the level's battle setups (as
// multi_arena.as picks them), or Stucco Courtyard's spawn markers
void SetUpMatch() {
    FindMarkers();
    spots.resize(0);
    spot_weapons.resize(0);
    spot_teams.resize(0); template_weights.resize(0);
    template_ffa = false; setup_points = 0; battle_name = "Courtyard";
    item_paths.resize(0);
    item_spots.resize(0);
    weapons_match = false;
    intro = "Time to fight!";
    array<int> used;

    if(!SetUpFromBattle(used)) {
        // Stucco Courtyard: a game type's markers first; a third of the
        // matches everyone gets the same weapon (arena_level.as)
        intro = "Welcome to the arena!";
        int game_type = rand() % 3;
        match_size = 2 + rand() % 3;
        int fighters = max(match_size, int(RosterSeats().length()));
        template_ffa = rand() % 3 == 0;
        int teams = min(max(2, fighters), rand() % 2 == 0 ? 2 : 3);
        for(int t = 0; t < teams; ++t) template_weights.insertLast(1);
        for(uint i = 0; i < marker_ids.length(); ++i) {
            if(marker_types[i] == game_type) {
                used.insertLast(i);
            }
        }
        if(rand() % 3 == 0) {
            spot_weapons.insertLast(WeaponPath("any"));
            weapons_match = true;
        }
    }

    string fallback_weapon = spot_teams.length() == 0 && spot_weapons.length() > 0 ? spot_weapons[0] : "";

    // The rest of the markers for more players (not on top of a used one)
    for(uint i = 0; i < marker_ids.length(); ++i) {
        bool near = false;
        for(uint k = 0; k < used.length(); ++k) {
            if(distance(MarkerPos(i), MarkerPos(used[k])) < 1.0f) {
                near = true;
            }
        }
        if(!near) {
            used.insertLast(i);
        }
    }

    while(spot_teams.length() < used.length()) spot_teams.insertLast(0);
    while(spot_weapons.length() < used.length()) spot_weapons.insertLast(fallback_weapon);
    arena_center = vec3(0.0f);
    for(uint k = 0; k < used.length(); ++k) {
        spots.insertLast(MarkerPos(used[k]));
        arena_center += spots[k];
    }
    if(spots.length() > 0) {
        arena_center /= float(spots.length());
    }
    max_points = setup_points > 0 ? setup_points : weapons_match ? 1 : 2;
}

bool SetUpFromBattle(array<int> &inout used) {
    array<JSONValue> choices;
    array<int> @ids = GetObjectIDs();
    for(uint i = 0; i < ids.length(); ++i) {
        ScriptParams@ params = ReadObjectFromID(ids[i]).GetScriptParams();
        if(!params.HasParam("Name") || params.GetString("Name") != "arena_battle" || !params.HasParam("Battles")) continue;
        JSON source = params.GetJSON("Battles");
        JSONValue battles = source.getRoot()["data"]["battles"];
        for(uint j = 0; j < battles.size(); ++j) if(battles[j]["teams"].size() >= 2) choices.insertLast(battles[j]);
    }
    if(choices.length() == 0) return false;
    array<int> eligible;
    for(uint i = 0; i < choices.length(); ++i)
        if(choices.length() == 1 || choices[i]["name"].asString() != last_battle_name) eligible.insertLast(i);
    if(eligible.length() == 0) eligible.insertLast(0);
    JSONValue battle = choices[eligible[rand() % int(eligible.length())]];
    battle_name = battle["name"].asString();
    last_battle_name = battle_name;
    JSONValue teams = battle["teams"];
    template_ffa = true;
    for(uint t = 0; t < teams.size(); ++t) {
        int members = int(teams[t]["members"].size());
        template_weights.insertLast(max(1, members));
        template_ffa = template_ffa && members <= 1;
    }
    int authored_size = 0;
    for(uint t = 0; t < template_weights.length(); ++t) authored_size += template_weights[t];
    match_size = min(4, max(2, authored_size));
    // Interleave the authored teams while retaining each position's team and
    // equipment. Larger rosters reuse safe neighbouring positions on that side.
    int most = 0;
    for(uint t = 0; t < teams.size(); ++t) most = max(most, int(teams[t]["members"].size()));
    for(int m = 0; m < most; ++m) {
        for(uint t = 0; t < teams.size(); ++t) {
            JSONValue members = teams[t]["members"];
            if(m >= int(members.size())) continue;
            JSONValue member = members[m];
            int at = marker_names.find(member["location"].asString());
            if(at < 0 || used.find(at) >= 0) continue;
            used.insertLast(at);
            spot_teams.insertLast(int(t + 1));
            string weapon = member.isMember("weapon") ? WeaponPath(member["weapon"].asString()) : "";
            spot_weapons.insertLast(weapon);
            weapons_match = weapons_match || weapon != "";
        }
    }
    JSONValue items = battle["items"];
    for(uint k = 0; k < items.size(); ++k) {
        JSONValue item = items[k];
        int at = marker_names.find(item["location"].asString());
        string weapon = WeaponPath(item.isMember("type") ? item["type"].asString() : "any");
        if(at >= 0 && weapon != "") {
            item_paths.insertLast(weapon); item_spots.insertLast(MarkerPos(at)); weapons_match = true;
        }
    }
    JSONValue attributes = battle["attributes"];
    if(attributes.isMember("intro") && attributes["intro"].asString() != "") intro = attributes["intro"].asString();
    if(attributes.isMember("rounds")) setup_points = min(10, max(1, atoi(attributes["rounds"].asString())));
    return true;
}

// The mode owns effective teams; the lobby remains a saved manual plan. Teams
// stay fixed through a match, then a new authored setup reshuffles the roster.
void ConfigureMatchTeams() {
    int n = int(roster.length());
    fighter_teams.resize(n);
    fighter_spots.resize(n);
    if(n == 0) return;
    array<int> order;
    for(int k = 0; k < n; ++k) order.insertLast(k);
    for(int k = n - 1; k > 0; --k) {
        int other = rand() % (k + 1), saved = order[k]; order[k] = order[other]; order[other] = saved;
    }
    if(LobbyGetForceTeams()) {
        int highest = 0;
        array<PlayerState>@ states = GetPlayerStates();
        for(uint k = 0; k < states.length(); ++k) highest = max(highest, states[k].team);
        int next_bot = highest + 1;
        for(int k = 0; k < n; ++k) {
            fighter_teams[k] = 0;
            for(uint j = 0; j < states.length(); ++j)
                if(states[j].avatar_id == roster[k]) fighter_teams[k] = states[j].team;
            if(fighter_teams[k] == 0) fighter_teams[k] = next_bot++;
        }
    } else if(template_ffa) {
        for(int k = 0; k < n; ++k) fighter_teams[order[k]] = k + 1;
    } else {
        int count = min(n, max(2, int(template_weights.length())));
        array<int> quota(count, 1);
        // Weighted apportionment preserves asymmetric authored battles while
        // ensuring every team has a fighter, even with fewer people than slots.
        for(int placed = count; placed < n; ++placed) {
            int best = rand() % count;
            float value = float(quota[best] + 1) / float(max(1, template_weights[best % int(template_weights.length())]));
            for(int t = 0; t < count; ++t) {
                float candidate = float(quota[t] + 1) / float(max(1, template_weights[t % int(template_weights.length())]));
                if(candidate < value) { best = t; value = candidate; }
            }
            ++quota[best];
        }
        int at = 0;
        for(int t = 0; t < count; ++t)
            for(int member = 0; member < quota[t]; ++member) fighter_teams[order[at++]] = t + 1;
    }
    array<int> distinct;
    for(int k = 0; k < n; ++k) if(distinct.find(fighter_teams[k]) < 0) distinct.insertLast(fighter_teams[k]);
    array<int> occupied(distinct.length(), 0);
    for(int k = 0; k < n; ++k) {
        int rank = distinct.find(fighter_teams[k]);
        array<int> candidates;
        if(!template_ffa && distinct.length() <= template_weights.length()) {
            int template_team = (LobbyGetForceTeams() ? rank : fighter_teams[k] - 1) % max(1, int(template_weights.length())) + 1;
            for(uint slot = 0; slot < spot_teams.length(); ++slot)
                if(spot_teams[slot] == template_team) candidates.insertLast(int(slot));
        }
        bool authored = candidates.length() > 0;
        if(!authored) for(uint slot = 0; slot < spots.length(); ++slot) candidates.insertLast(int(slot));
        int ordinal = occupied[rank]++;
        if(candidates.length() > 0) {
            int offset = authored ? ordinal : rank + ordinal * int(distinct.length());
            // Encodes overflow separately so SpotFor can find a clear neighbour.
            fighter_spots[k] = candidates[offset % int(candidates.length())] +
                (offset / int(candidates.length())) * int(spots.length());
        } else fighter_spots[k] = 0;
    }
    LobbyAssignModeTeams(roster, fighter_teams);
    string report = "ohl arena teams " + battle_name;
    for(int k = 0; k < n; ++k) report += " #" + roster[k] + " " + fighter_teams[k];
    level.SendMessage(report);
}

string MatchDescription() {
    if(fighter_teams.length() == 0) return "";
    array<int> labels, counts;
    for(uint k = 0; k < fighter_teams.length(); ++k) {
        int at = labels.find(fighter_teams[k]);
        if(at < 0) { labels.insertLast(fighter_teams[k]); counts.insertLast(1); }
        else ++counts[at];
    }
    if(labels.length() == fighter_teams.length()) return "Free-for-all: " + fighter_teams.length() + " fighters.";
    string format = "Teams: ";
    for(uint k = 0; k < counts.length(); ++k) format += (k > 0 ? " vs " : "") + counts[k];
    return format + ".";
}

// Player k's spot, facing the middle; more players than spots: beside them
void SpotFor(uint k, vec3 &out pos, vec3 &out facing) {
    if(spots.length() == 0) {
        pos = vec3(0.0f);
        facing = vec3(0.0f, 0.0f, 1.0f);
        return;
    }
    uint slot = k < fighter_spots.length() ? uint(fighter_spots[k]) : k;
    vec3 base = spots[slot % spots.length()];
    vec3 to_middle = arena_center - base;
    to_middle.y = 0.0f;
    facing = length(to_middle) > 0.1f ? normalize(to_middle) : vec3(0.0f, 0.0f, 1.0f);
    // Some authored markers are feet-level, embedded in the arena's rock
    // floor (Rock Cave also has a second terrain surface underneath it).
    // Resolve the original spots too: raw teleports can put a character
    // through that upper collision surface.
    pos = Online_SafeSpotNear(base, facing, int(slot / spots.length()));
}

// ---- shown ----------------------------------------------------------------

void Show(const string &in text) {
    ShowLocal(text);
    level.SendMessage("ohl arena text " + text);
}

void ShowLocal(const string &in text) {
    if(text == shown_text) {
        return;
    }
    shown_text = text;
    if(text != "") {
        DrawText(text_id, text, 48);
    }
}

void SendScore() {
    string msg = "ohl arena score " + max_points;
    for(uint k = 0; k < roster.length(); ++k) {
        msg += " #" + roster[k] + " " + points[k];
    }
    level.SendMessage(msg);

    score_ids = roster;
    score_points = points;
    score_max = max_points;
    score_dirty = true;
}

// "ohl arena score <max> #<char> <points> ..."
void ReceiveScore(const string &in msg) {
    TokenIterator token_iter;
    token_iter.Init();
    for(int k = 0; k < 4; ++k) {
        if(!token_iter.FindNextToken(msg)) {
            return;
        }
    }
    score_max = atoi(token_iter.GetToken(msg));
    score_ids.resize(0);
    score_points.resize(0);
    while(token_iter.FindNextToken(msg)) {
        string id = token_iter.GetToken(msg);
        if(!token_iter.FindNextToken(msg)) {
            break;
        }
        if(id.length() > 1 && id.substr(0, 1) == "#") {
            score_ids.insertLast(atoi(id.substr(1)));
            score_points.insertLast(atoi(token_iter.GetToken(msg)));
        }
    }
    score_dirty = true;
}

void DrawScoreboard() {
    TextCanvasTexture @text = level.GetTextElement(score_id);
    text.ClearTextCanvas();
    if(score_ids.length() == 0) {
        text.UploadTextCanvasToTexture();
        return;
    }
    string font_str = level.GetPath("font");
    TextStyle style;
    style.font_face_id = GetFontFaceID(font_str, 36);
    text.SetPenColor(255, 255, 255, 255);
    text.SetPenRotation(0.0f);

    int own = GetPlayerCharacterID();
    int own_id = own != -1 ? ReadCharacter(own).GetID() : -1;
    int line = 0;
    text.SetPenPosition(vec2(0, kScoreLineHeight));
    text.AddText(score_max == 1 ? "First to 1 point" : "First to " + score_max + " points", style, UINT32MAX);
    for(uint k = 0; k < score_ids.length() && k < kMaxScoreRows; ++k) {
        ++line;
        text.SetPenPosition(vec2(0, (1 + line) * kScoreLineHeight));
        int team = LobbyGetAssignedTeam(score_ids[k]);
        text.AddText((team > 0 ? "T" + team + " " : "") + PlayerName(score_ids[k]) + (score_ids[k] == own_id ? " (you)" : "") + ":  " + score_points[k], style, UINT32MAX);
    }
    text.UploadTextCanvasToTexture();
}

int TextInit(int width, int height) {
    int id = level.CreateTextElement();
    TextCanvasTexture @text = level.GetTextElement(id);
    text.Create(width, height);
    return id;
}

void DrawText(int id, const string &in str, int size) {
    TextCanvasTexture @text = level.GetTextElement(id);
    text.ClearTextCanvas();
    string font_str = level.GetPath("font");
    TextStyle style;
    style.font_face_id = GetFontFaceID(font_str, size);
    text.SetPenPosition(vec2(0, 256));
    text.SetPenColor(255, 255, 255, 255);
    text.SetPenRotation(0.0f);
    text.AddTextMultiline(str, style, UINT32MAX);
    text_ink_height = min(512.0f, text.GetPenPosition().y + size * 0.25f) - kTextInkTop;
    text.UploadTextCanvasToTexture();
}

// ---- the crowd (multi_arena.as) -------------------------------------------

void UpdateCrowd() {
    float total_char_speed = 0.0f;
    int num = GetNumCharacters();
    for(int i = 0; i < num; ++i) {
        MovementObject@ char = ReadCharacter(i);
        if(char.GetIntVar("knocked_out") == _awake) {
            total_char_speed += length(char.velocity);
        }
    }
    float excitement_decay_rate = 3.0f / (1.0f + total_char_speed / 14.0f);
    audience_excitement *= pow(0.05f, 0.001f * excitement_decay_rate);
    float target_crowd_cheer_amount = audience_excitement * 0.1f + 0.15f;
    crowd_cheer_vel += (target_crowd_cheer_amount - crowd_cheer_amount) * time_step * 10.0f;
    if(crowd_cheer_vel > 0.0f) {
        crowd_cheer_vel *= 0.99f;
    } else {
        crowd_cheer_vel *= 0.95f;
    }
    crowd_cheer_amount += crowd_cheer_vel * time_step;
    crowd_cheer_amount = max(crowd_cheer_amount, 0.1f);
    if(audience_sound_handle != -1) {
        SetSoundGain(audience_sound_handle, crowd_cheer_amount * 2.0f);
        SetSoundPitch(audience_sound_handle, min(0.8f + crowd_cheer_amount * 0.5f, 1.2f));
    }
}
