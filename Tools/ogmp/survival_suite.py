"""Team waves, saved runs, battery celebrations and the end-of-run scoreboard."""

import json
import re
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA_MAP, SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _ready, _rows
from .scenarios import script_errors


def survival_waves(ctx):
    keys = {**RENDER_KEYS, "player_invincible": "true", "difficulty_scaling": "true", "campaign_weapon_scaling": "true"}
    host = ctx.instance("survival_director_host", width=640, height=360, keys={**keys, "mp_port": "29264"})
    guest = ctx.instance("survival_director_guest", width=640, height=360, keys={**keys, "mp_port": "29265"})
    wait = lambda fn, label: host.bridge.wait(fn, 50, step=.12, what=label)

    def script(text):
        return _code(host, text, "level", "level_specific")

    def state(peer=host):
        value = _probe(peer, 'SessionGetModData("og-online-survival")', "level")
        return json.loads(value)

    def enemies(peer=host):
        return [row for row in peer.bridge.state() if "survival_enemies" in row["teams"] and row["enabled"] and row["knocked_out"] == 0]

    def freeze():
        for row in enemies():
            _code(host, 'this_mo.static_char = true; this_mo.ReceiveScriptMessage("set_combat_allowed false");', row["id"])

    def kill(peer, avatar):
        _code(peer, "SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", avatar)

    def all_spawned(number):
        snapshot = state()
        return (snapshot.get("launched") == number and snapshot.get("enemies", 0) > 0
                and len(enemies()) == snapshot["enemies"])

    def next_wave(number):
        script("due = the_time;")
        wait(lambda: all_spawned(number), f"wave {number} deploys its whole formation")
        freeze()

    def hud(peer):
        return _probe(peer, "status_text", "level", "level_specific")

    def countdown(peer):
        match = re.search(r"next wave in (\d+)s", hud(peer), re.IGNORECASE)
        return int(match.group(1)) if match else None

    def hear_countdown():
        prefix = "ohl survival countdown "
        peers = (host, guest)
        heard = {peer: [] for peer in peers}
        for peer in peers:
            peer.bridge.call("level_messages", prefix=prefix)
        script("due = the_time + 3.4f;")

        def collect():
            for peer in peers:
                heard[peer].extend(peer.bridge.call("level_messages", prefix=prefix)["messages"])
            return all(len(ticks) >= 3 for ticks in heard.values())

        wait(collect, "last three countdown cues reach both peers")
        for ticks in heard.values():
            assert ticks == [prefix + str(second) for second in (3, 2, 1)], ticks

    def unarmed_opening():
        for row in enemies():
            assert all(slot < 0 for slot in row["weapon_slots"][:4]), row
            assert any(kind in row["char_path"].lower() for kind in ("rat", "rabbit")), row
            params = f'ReadObjectFromID({row["id"]}).GetScriptParams()'
            assert _probe(host, f'{params}.HasParam("Wave Difficulty Weapons") && '
                          f'{params}.GetInt("Wave Difficulty Weapons") == 0', "level") == "true"

    def confirm_teams():
        team1 = next(row["avatar_id"] for row in _rows(host) if row["host"])
        team2 = next(row["avatar_id"] for row in _rows(host) if not row["host"])
        for row in enemies():
            owned = "lobby_team_2" in row["teams"]
            for target, allied in ((team1, not owned), (team2, owned)):
                assert _probe(host, f'ReadCharacterID({row["id"]}).OnSameTeam(ReadCharacterID({target}))', "level") == str(allied).lower()

    _code(host, 'LobbyBeginHost();')
    guest.bridge.call("connect", ip="127.0.0.1:29264")
    wait(lambda: len(_rows(host)) == 2, "guest joins")
    visitor = next(p["id"] for p in _rows(host) if not p["host"])
    _code(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false);'
          f'LobbySetPlayerTeam({visitor},2);LobbySetModeSetting("progression",0);LobbySetModeSetting("survival_seed",7301);LobbyStart();')
    for peer in (host, guest):
        _ready(peer, SANDBOX_MAP)
    wait(lambda: len(enemies()) == 2, "first wave has an enemy for each team")
    freeze()
    confirm_teams()
    wait(lambda: len(enemies(guest)) == 2 and state(guest).get("launched") == 1, "guest sees both host-owned enemies")
    for peer, viewer_team in ((host, 1), (guest, 2)):
        for enemy in enemies(peer):
            neutral = f"lobby_team_{viewer_team}" in enemy["teams"]
            assert _probe(peer, f'IsNeutralWaveEnemy(ReadCharacterID({enemy["id"]}),{viewer_team})',
                          "level", "level_specific") == str(neutral).lower()
    assert state()["wave_count"] == 1
    # Exercise the actual director, including exact spending at the actor cap,
    # equally matched teams and budget growth within every battery.
    script('array<int> batteries={1,2,3,4,8,16,40,80,150}; int errors=0; '
           'for(uint b=0;b<batteries.length();++b){int number=batteries[b];'
           'for(int members=1;members<=5;members+=4){int previous=0;'
           'for(int wave=1;wave<=WavesForBattery(number);++wave){'
           'int cost=WaveBudget(number,wave,members); if(cost<=previous)++errors; previous=cost;'
           'array<Raider@> a=PlanWave(8901,number,wave,members,1), z=PlanWave(8901,number,wave,members,2);'
           'int spent=0; if(a.length()!=z.length() || a.length()>10)++errors;'
           'for(uint j=0;j<a.length();++j){Raider@ r=a[j]; spent+=species_cost[r.kind];'
           'if(r.weapon>=0)spent+=weapon_cost[r.weapon];'
           'if(number<4 && (r.kind>2 || r.weapon>=0))++errors;'
           'if(z[j].kind!=r.kind || z[j].weapon!=r.weapon)++errors;}'
           'if(spent!=cost)++errors;}}} level.SendMessage("test_budget " + errors);')
    assert host.bridge.call("level_messages", prefix="test_budget ")["messages"] == ["test_budget 0"]
    script('for(int seed=0;seed<24;++seed){array<Raider@> p=PlanWave(seed,2,1,1,1);'
           'level.SendMessage("test_draw " + p.length());}')
    sizes = set(host.bridge.call("level_messages", prefix="test_draw ")["messages"])
    assert sizes == {"test_draw 1", "test_draw 2"}, sizes  # one rabbit or two rats for two points
    for number, waves in ((3, 2), (4, 3), (7, 3), (8, 4), (15, 4), (16, 5), (32, 6), (64, 7)):
        assert _probe(host, f"WavesForBattery({number})", "level", "level_specific") == str(waves)
    for number in (1, 10, 19, 20, 75):
        growth = min(1.0, (number - 1) / 19.0)
        for axis, expected in (("x", 12 + 28 * growth), ("y", 20 + 44 * growth)):
            value = float(_probe(host, f"SpawnRadii({number}).{axis}", "level", "level_specific"))
            assert abs(value - expected) < 0.001, (number, axis, value, expected)

    # Sample actual grounded candidates before actors can move in from the ring.
    # This large hillside has traversable ground in both the opening and mature rings.
    anchor = host.bridge.me()["id"]
    for number, low, high in ((1, 12, 20), (20, 40, 64)):
        script(f'array<int> targets = {{{anchor}}}; vec3 center = ReadCharacterID({anchor}).position; '
               f'vec3 spot, rally; for(int i=0; i<8; ++i) if(SpawnSpot(targets, center, SpawnRadii({number}), 0, spot, rally, i)) '
               '{level.SendMessage("survival_ring " + sqrt(xz_distance_squared(spot, center)) + " " + '
               'distance(center, rally)); break;}')
        hits = host.bridge.call("level_messages", prefix="survival_ring")["messages"]
        assert hits, (number, "no reachable ground in the preferred ring")
        distance, search_distance = map(float, hits[-1].split()[1:])
        assert low <= distance <= high and search_distance >= 1.0, (number, distance, search_distance)
    unarmed_opening()
    cue = "ohl survival battery_complete"
    peers = (host, guest)
    for peer in peers:
        peer.bridge.call("level_messages", prefix=cue)
    for row in enemies():
        kill(host, row["id"])
    for peer in peers:
        wait(lambda: peer.bridge.call("level_messages", prefix=cue)["messages"] == [cue],
             "battery win cue reaches each peer once")
    wait(lambda: all(state(peer).get("phase") == 4 for peer in peers), "win camera on host and guest")
    wait(lambda: state().get("phase") == 2, "first battery enters its break after the camera")
    hear_countdown()
    wait(lambda: state().get("battery") == 2 and state()["phase"] == 0, "second battery prepares")
    next_wave(1)
    unarmed_opening()
    for peer in (host, guest):
        wait(lambda: (countdown(peer) or 0) > 1, "next-wave timer visible")
    initial = [countdown(peer) for peer in (host, guest)]
    wait(lambda: all(0 < (countdown(peer) or 99) < first for peer, first in zip((host, guest), initial)),
         "next-wave countdown decreases on host and guest while both enemies live")
    hear_countdown()
    wait(lambda: all_spawned(2), "next wave arrives after its third cue")
    freeze()
    confirm_teams()
    for peer in (host, guest):
        wait(lambda: "2/2" in hud(peer) and countdown(peer) is None, "last wave hides next-wave timer")
    card = Path(host.writedir, "Data/SessionSaves/slot1.json")
    script("SessionSave(1);")
    wait(card.is_file, "manual save in two overlapping waves")
    record = json.loads(card.read_text())
    saved = record["session_mods"]["og-online-survival"]["data"]
    counts = [len(w["remaining"]) for w in saved["waves"]]
    assert len(counts) == 2 and all(n >= 2 for n in counts) and saved["phase"] == 1, saved
    assert [score["peak_members"] for score in saved["scores"]] == [1, 1]
    assert all(len(raider) == 4 for w in saved["waves"] for raider in w["remaining"]), saved
    team_one_count = sum(counts) // 2

    # An earlier release could already have two waves alive in its first battery.
    saved["battery"] = 1
    saved["wave_count"] = 3
    saved.pop("scores", None)
    # Advance one genuinely old wave alongside a persisted seeded wave.
    for raider in saved["waves"][0]["remaining"]:
        del raider[2:]
    card.write_text(json.dumps(record))
    manual_save = card.read_bytes()

    # Down the owner on the owner's machine. Team 1's enemies remain alive.
    kill(guest, guest.bridge.me()["id"])
    wait(lambda: state().get("eliminated") == [2] and len(enemies()) == team_one_count, "defeated team's two groups despawn")
    wait(lambda: len(enemies(guest)) == team_one_count, "despawn replicated to guest")
    assert all("lobby_team_2" in row["teams"] for row in enemies())
    for row in enemies():
        kill(host, row["id"])
    wait(lambda: state().get("phase") == 2, "battery clears only after surviving team clears its waves")
    assert state()["enemies"] == 0
    owner = next(p["id"] for p in _rows(host) if p["host"])
    assert _probe(host, f"LobbySetPlayerTeam({owner},2)", "level") == "true"
    time.sleep(1)
    assert state()["battery"] == 2 and state()["phase"] == 2, "reassigning the sole survivor ended the break"
    assert _probe(host, f"LobbySetPlayerTeam({owner},1)", "level") == "true"
    script("due = the_time;")
    wait(lambda: state().get("battery") == 3 and guest.bridge.me()["knocked_out"] == 0, "eliminated team returns for next battery")

    # Let the normal preparation timer run so the host sees the guest's revived
    # avatar before the first wave evaluates which teams are still alive.
    wait(lambda: all_spawned(1), "third battery deploys for both teams")
    freeze()
    unarmed_opening()
    next_wave(2)
    unarmed_opening()

    script("SessionLoad(1);")
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)), "save restored its lobby")
    _code(host, "LobbyStart();")
    for peer in (host, guest):
        _ready(peer, SANDBOX_MAP)
    wait(lambda: all_spawned(2), "restored both surviving waves and targets")
    freeze()
    confirm_teams()
    assert state()["wave_count"] == 3
    restored_waves = state()["waves"]
    assert restored_waves[1]["remaining"] == saved["waves"][1]["remaining"]
    assert all(len(fighter) == 4 for fighter in restored_waves[0]["remaining"])
    next_wave(3)
    after_first_wave = len(enemies()) - counts[0]
    for row in enemies():
        params = f'ReadObjectFromID({row["id"]}).GetScriptParams()'
        if _probe(host, f'{params}.GetInt("Survival Wave")', "level") == "1":
            kill(host, row["id"])
    wait(lambda: len(enemies()) == after_first_wave, "both teams clear an overlapping saved wave")
    kill(guest, guest.bridge.me()["id"])
    wait(lambda: state().get("eliminated") == [2] and len(enemies()) == after_first_wave // 2,
         "the defeated guest's remaining enemies despawn while the host plays")
    remaining = {r["common_id"] for r in enemies()}
    kill(host, host.bridge.me()["id"])
    wait(lambda: all(state(peer).get("phase") == 3 for peer in peers), "last team falls on both peers")
    for peer in peers:
        assert {r["common_id"] for r in enemies(peer)} == remaining, "opponents disappeared on the last knockout"
    wait(lambda: all(state(peer).get("phase") == 5 for peer in peers), "both peers reach the final scoreboard")
    for peer in peers:
        wait(lambda: bool(_probe(peer, "score_text", "level", "level_specific")), "visible results")
    score = state()["scores"]
    assert state(guest)["scores"] == score
    assert {r["team"] for r in score} == {1, 2}
    assert all(r["waves"] >= 1 and r["kills"] >= 1 for r in score), score
    ctx.shot(guest, "survival_results")
    guest.bridge.call("pointer", x=250, y=170, buttons=4)
    guest.bridge.call("pointer", x=250, y=170, buttons=0)
    # Advancing the old timeout or a guest clicking must leave the host in control.
    script("due = the_time - 60;")
    time.sleep(.5)
    assert state()["phase"] == 5
    host.bridge.call("pointer", x=250, y=170, buttons=4)
    host.bridge.call("pointer", x=250, y=170, buttons=0)
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in peers), "game over returns the intact party to its lobby")
    assert card.read_bytes() == manual_save
    assert len(_rows(host)) == len(_rows(guest)) == 2

    # Magma Arena supplies spawn markers rather than an authored player.
    # Its first marker has a roof directly above the playing floor.
    _code(host, f'LobbySetSelection("survival","","{ARENA_MAP}",false); LobbyStart();')
    for peer in (host, guest):
        _ready(peer, ARENA_MAP)
    wait(lambda: len(enemies()) == len(enemies(guest)) == 2, "marker-only map creates a roster and two teams of enemies")
    script('array<int>@ markers=GetObjectIDsType(_placeholder_object); for(uint i=0;i<markers.length();++i){'
           'Object@ marker=ReadObjectFromID(markers[i]); ScriptParams@ p=marker.GetScriptParams();'
           'if(p.HasParam("Name") && p.GetString("Name")=="arena_spawn"){'
           'level.SendMessage("survival_marker "+distance(home,marker.GetTranslation()));break;}}')
    marker = host.bridge.call("level_messages", prefix="survival_marker")["messages"][-1]
    assert float(marker.split()[-1]) < 3, marker

    # Two teammates earn a four-point opening wave in battery two. Losing the
    # guest before its finale must still produce five points of opponents.
    assert _probe(host, f"LobbySetPlayerTeam({visitor},1)", "level") == "true"
    wait(lambda: state().get("eliminated") == [2], "guest joins the host's team")
    for row in enemies():
        kill(host, row["id"])
    wait(lambda: state().get("phase") == 2, "team clears the opener")
    script("due = the_time;")
    wait(lambda: state().get("battery") == 2 and state().get("launched") == 1, "two-person formation")
    assert next(score for score in state()["scores"] if score["team"] == 1)["peak_members"] == 2
    kill(guest, guest.bridge.me()["id"])
    wait(lambda: _probe(host, "int(Members(GetPlayerStates(),1,true).length())", "level", "level_specific") == "1",
         "host sees the guest fall")
    script("due = the_time;")
    wait(lambda: state().get("launched") == 2, "finale launches for the survivor")
    final_wave = next(w for w in state()["waves"] if w["number"] == 2)
    assert sum(1 if r[2] == 1 else 2 for r in final_wave["remaining"]) == 5, final_wave
    for peer in (host, guest):
        assert not script_errors(peer), (peer.name, script_errors(peer))
    return {"passed": True, "metrics": {"teams": 2, "overlapping_waves_restored": 2,
            "other_team_despawns": 2, "enforced_break": True, "game_over_returns_to_lobby": True, "marker_only_map": ARENA_MAP}}
