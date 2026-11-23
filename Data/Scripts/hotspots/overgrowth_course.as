// Co-op climbing death floors follow each human's own route progress. Story
// goals remain shared; reaching a higher ledge must not arm a floor around a
// teammate who is still climbing below it. Keys are persistent lobby seats,
// not character-array indices or the current local/menu player.
class CoopCourse {
    int seat = -1;
    int avatar = -1;
    int stage = 0;
    bool seeded = false;
    bool was_down = false;
    array<int> visited;
}

array<CoopCourse@> coop_courses;
float coop_course_next_seed = 0.0f;
array<int> coop_course_pending_targets;
array<int> coop_course_pending_anchors;

CoopCourse@ CoopCourseForSeat(int seat) {
    for(uint i = 0; i < coop_courses.length(); ++i)
        if(coop_courses[i].seat == seat) return coop_courses[i];
    return null;
}

CoopCourse@ CoopCourseForCharacter(int avatar) {
    for(uint i = 0; i < coop_courses.length(); ++i)
        if(coop_courses[i].avatar == avatar) return coop_courses[i];
    return null;
}

bool CoopCourseIsLocal(int seat) {
    array<LobbyPlayer> players = LobbyPlayers();
    for(uint i = 0; i < players.length(); ++i)
        if(int(players[i].id) == seat) return players[i].local;
    return false;
}

int CoopCourseAnchorStage(int avatar) {
    if(!MovementObjectExists(avatar)) return progress;
    vec3 position = ReadCharacterID(avatar).position;
    float nearest = -1.0f;
    int stage = progress;
    for(uint i = 0; i < coop_courses.length(); ++i) {
        CoopCourse@ other = coop_courses[i];
        if(other.avatar == avatar || !other.seeded || !MovementObjectExists(other.avatar)) continue;
        MovementObject@ character = ReadCharacterID(other.avatar);
        if(character.GetIntVar("knocked_out") != _awake) continue;
        float dist = distance_squared(character.position, position);
        if(nearest < 0.0f || dist < nearest) {
            nearest = dist;
            stage = other.stage;
        }
    }
    return stage;
}

void CoopCourseReset() {
    coop_courses.resize(0);
    coop_course_pending_targets.resize(0);
    coop_course_pending_anchors.resize(0);
    coop_course_next_seed = 0.0f;
}

void CoopCourseUpdate() {
    if(!Online_IsActive() || queued_goal_check) return;
    array<LobbyPlayer> players = LobbyPlayers();
    for(uint i = 0; i < players.length(); ++i) {
        if(players[i].bot) continue;
        int seat = int(players[i].id);
        int avatar = Online_GetPlayerCharacter(players[i].host ? 0 : seat);
        if(!MovementObjectExists(avatar)) continue;
        bool down = ReadCharacterID(avatar).GetIntVar("knocked_out") != _awake;
        CoopCourse@ course = CoopCourseForSeat(seat);
        if(course is null) {
            @course = CoopCourse();
            course.seat = seat;
            coop_courses.insertLast(course);
        }
        if(course.avatar != avatar) {
            bool first_actor = course.avatar < 0;
            course.avatar = avatar;
            course.was_down = down;
            course.visited.resize(0);
            if(Online_IsHosting()) {
                course.stage = CoopCourseAnchorStage(avatar);
                course.seeded = true;
                coop_course_next_seed = 0.0f;
            } else if(!first_actor) {
                course.stage = 0;
                course.seeded = false;
            }
        } else if(course.was_down && !down) {
            // A native revive places the character beside a living teammate.
            // The explicit rebase message also handles still-awake placement.
            course.stage = CoopCourseAnchorStage(avatar);
            course.seeded = true;
            course.visited.resize(0);
            coop_course_next_seed = 0.0f;
        }
        course.was_down = down;
    }
    for(int i = int(coop_courses.length()) - 1; i >= 0; --i) {
        bool present = false;
        for(uint p = 0; p < players.length(); ++p)
            present = present || (!players[p].bot && int(players[p].id) == coop_courses[i].seat);
        if(!present) coop_courses.removeAt(i);
    }
    for(int i = int(coop_course_pending_targets.length()) - 1; i >= 0; --i) {
        CoopCourse@ target = CoopCourseForCharacter(coop_course_pending_targets[i]);
        CoopCourse@ anchor = CoopCourseForCharacter(coop_course_pending_anchors[i]);
        if(target is null || anchor is null) continue;
        target.stage = anchor.stage;
        target.seeded = anchor.seeded;
        target.visited.resize(0);
        coop_course_pending_targets.removeAt(i);
        coop_course_pending_anchors.removeAt(i);
        coop_course_next_seed = 0.0f;
    }
    if(Online_IsHosting() && the_time >= coop_course_next_seed) {
        coop_course_next_seed = the_time + 2.0f;
        for(uint i = 0; i < coop_courses.length(); ++i) {
            CoopCourse@ course = coop_courses[i];
            if(course.seeded)
                level.SendMessage("ohl og_course_seed #" + hotspot.GetID() + " " + course.seat + " " + course.stage);
        }
    }
}

// Native safe-ground checks use the same per-character gate as the hotspot.
int CoopCourseProgressForCharacter(int avatar) {
    if(!Online_IsActive()) return progress;
    CoopCourse@ course = CoopCourseForCharacter(avatar);
    if(course !is null) return min(progress, course.stage);
    array<LobbyPlayer> players = LobbyPlayers();
    for(uint i = 0; i < players.length(); ++i)
        if(!players[i].bot && Online_GetPlayerCharacter(players[i].host ? 0 : int(players[i].id)) == avatar) return 0;
    return progress;  // NPCs/bots retain the mode's original hazard policy.
}

void CoopCourseSeed(int seat, int stage) {
    CoopCourse@ course = CoopCourseForSeat(seat);
    if(course is null) {
        @course = CoopCourse();
        course.seat = seat;
        coop_courses.insertLast(course);
    }
    // Once admitted, a local human advances by their own contacts. A periodic
    // shared snapshot must not move their death floor with someone else.
    if(!course.seeded) course.stage = max(course.stage, max(0, stage));
    else if(!CoopCourseIsLocal(seat)) course.stage = max(0, stage);
    course.seeded = true;
}

void CoopCourseCheckpoint(int checkpoint, int avatar, int source) {
    CoopCourseUpdate();
    CoopCourse@ course = CoopCourseForCharacter(avatar);
    if(course is null || (source >= 0 && course.visited.find(source) >= 0)) return;
    if(source >= 0) course.visited.insertLast(source);
    // Goal numbers may include fights/dialogues or repeat a checkpoint number
    // later. Distinct authored checkpoint volumes advance to the next matching
    // reach goal; revisiting the same volume cannot jump a later occurrence.
    for(int goal = course.stage; params.HasParam("goal_" + goal); ++goal) {
        TokenIterator tokens;
        tokens.Init();
        string text = params.GetString("goal_" + goal);
        if(!tokens.FindNextToken(text)) continue;
        string type = tokens.GetToken(text);
        if((type == "reach" || type == "reach_skippable") && tokens.FindNextToken(text) &&
           atoi(tokens.GetToken(text)) == checkpoint) {
            course.stage = goal + 1;
            coop_course_next_seed = 0.0f;
            return;
        }
    }
}

void CoopCourseRebase(int avatar, int anchor) {
    CoopCourseUpdate();
    CoopCourse@ course = CoopCourseForCharacter(avatar);
    CoopCourse@ beside = CoopCourseForCharacter(anchor);
    if(course is null || beside is null) {
        if(avatar < 0 || anchor < 0) return;
        int pending = coop_course_pending_targets.find(avatar);
        if(pending < 0) {
            coop_course_pending_targets.insertLast(avatar);
            coop_course_pending_anchors.insertLast(anchor);
        } else coop_course_pending_anchors[pending] = anchor;
        return;
    }
    course.stage = beside is null ? CoopCourseAnchorStage(avatar) : beside.stage;
    course.seeded = true;
    course.visited.resize(0);
    coop_course_next_seed = 0.0f;
}
