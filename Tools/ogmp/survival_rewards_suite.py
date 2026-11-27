"""Survival awards each team's wave, freezes overlapping opponents and stalls for chosen upgrades."""
import json
import math
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _join_pad, _ready, _rows
from .scenarios import script_errors


def survival_rewards(ctx):
    host = ctx.instance("survival_rewards_host", width=1024, height=720, keys={
        **RENDER_KEYS, "mp_port": "29376", "player_invincible": "true", "difficulty_scaling": "true"})
    bridge = host.bridge

    def wait(condition, label):
        return bridge.wait(condition, 40, step=.1, what=label)

    def director(code):
        return _code(host, code, "level", "level_specific")

    def data(seat):
        return bridge.call("progression", actor=seat)

    def balance(seat):
        wallet = data(seat)["view"]
        return wallet["held"], wallet["pooled"]

    def wave_estimate_visible(seat):
        scale = data(seat)["view"]["xp_scale"] or {}
        return scale.get("encounter_label") == "Active waves" and scale.get("world_remaining", 0) > 0

    def panels():
        return bridge.status()["progression_ui"]

    def actors():
        return {c["id"]: c for c in bridge.state() if "survival_enemies" in c["teams"] and c["knocked_out"] == 0}

    def add_wave(number, prior):
        director("due = the_time;")
        def deployed():
            snapshot = json.loads(_probe(host, 'SessionGetModData("og-online-survival")', "level"))
            return snapshot.get("launched") == number and snapshot["enemies"] == len(actors()) and bool(actors().keys() - prior)
        wait(deployed, f"wave {number} spawns")
        fresh = actors().keys() - prior
        director("due = the_time + 600;")
        result = {1: [], 2: []}
        for actor in fresh:
            _code(host, 'this_mo.static_char=true; this_mo.ReceiveScriptMessage("set_combat_allowed false");', actor)
            target = int(_probe(host, f'ReadObjectFromID({actor}).GetScriptParams().GetInt("Survival Team")', "level"))
            result[target].append(actor)
        return result

    def kill(ids):
        for actor in ids:
            _code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", actor)

    def injure(actor, health=.4):
        _code(host, f"permanent_health={health}f;temp_health={health}f;blood_health=.95f;block_health=1.0f;blood_damage=.01f;", actor)

    def fully_healed(actor):
        row = next(row for row in bridge.state() if row["id"] == actor)
        return all(row[key] == 1 for key in ("permanent_health", "temp_health", "blood_health"))

    def vitality(actor):
        return actors()[actor]["combat_stats"]["campaign_vitality_mult"]

    def pressure():
        return bridge.status()["online"]["challenge"]["pressure"]

    _code(host, "LobbyBeginHost();")
    _join_pad(host, 0, 2)
    _code(host, "SetControllerPlayer(0);")
    seats = sorted(_rows(host), key=lambda p: p["local_index"])
    first, second = [p["id"] for p in seats]
    assert _probe(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false)') == "true"
    assert _probe(host, f"LobbySetPlayerTeam({second},2)") == "true"
    for key, value in {"progression": 1, "progression_shop": 1, "progression_free_buffs": 1,
                       "progression_shared": 1, "progression_gain_damage": 1, "progression_gain_kill": 0,
                       "progression_loss_damage": 1, "progression_gain_round": 1, "progression_gain_match": 1,
                       "challenge_time": 0, "challenge_round": 2, "challenge_performance": 1}.items():
        assert _probe(host, f'LobbySetModeSetting("{key}",{value})') == "true", key
    _code(host, "LobbyStart();")
    _ready(host, SANDBOX_MAP)
    # Give both isolated test seats guaranteed reward draws using their saved Luck.
    wait(lambda: len(panels()) == 2, "both local wallets")
    director("due = the_time + 600; SessionSave(2);")
    path = Path(host.writedir, "Data/SessionSaves/slot2.json")
    wait(path.is_file, "save the lobby and progression roster")
    card = json.loads(path.read_text())
    accounts = card["session_mods"]["og-online-progression"]["runtime"]["players"]
    assert {r["id"] for r in accounts} == {first, second}
    for account in accounts:
        account["luck"] = 50
    # Start at the first three-wave battery to exercise out-of-order clears.
    director_save = card["session_mods"]["og-online-survival"]["data"]
    assert director_save["launched"] == 0
    director_save["battery"] = 4
    director_save["wave_count"] = 3
    path.write_text(json.dumps(card))
    director("SessionLoad(2);")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "saved lobby restored")
    _code(host, "LobbyStart();")
    _ready(host, SANDBOX_MAP)
    wait(lambda: _probe(host, "host_ready", "level", "level_specific") == "true", "director ready")
    heroes = {p["team"]: p["avatar_id"] for p in _rows(host)}
    first_wave = add_wave(1, set())
    # Both split-screen cameras look at the same raider. Only the other team's
    # screen should mark it; swapping the subject must swap the marked screen.
    for target_team, marked_screen in ((2, 1), (1, 2)):
        subject = first_wave[target_team][0]
        x, y, z = actors()[subject]["pos"]
        bridge.call("view", pos=[x, y + 2.5, z + 3], target=[x, y + .65, z], gui=True, pause=False)

        def marker_screens():
            director(f'string shown=""; for(uint i=0; i<neutral_markers.length(); ++i) '
                     f'if(neutral_markers[i].actor == {subject}) shown += neutral_markers[i].screen; '
                     'level.SendMessage("test_neutral " + shown);')
            return bridge.call("level_messages", prefix="test_neutral ")["messages"][-1]

        wait(lambda: marker_screens() == f"test_neutral {marked_screen}", "neutral marker follows the viewing team")
        ctx.shot(host, f"neutral_raider_screen_{marked_screen}")
    bridge.call("view", clear=True)
    wait(lambda: all(wave_estimate_visible(seat) for seat in (first, second)), "both teams see their active wave estimates")
    original = first_wave[1][0]
    wait(lambda: "campaign_vitality_mult" in actors()[original]["combat_stats"], "initial enemy parameters")
    second_wave = add_wave(2, set(actors()))
    for team in (1, 2):
        director(f"ProgressionReportHit({first_wave[team][0]},{heroes[team]},.5f,false);")
    first_pool, second_pool = balance(first)[1], balance(second)[1]
    assert first_pool > 0 and second_pool > 0
    kill(second_wave[1])
    wait(lambda: balance(first) == (25 + first_pool, 0), "team 1 banks its own second wave")
    assert balance(second) == (0, second_pool), "team 2 also banked on team 1's wave clear"
    assert pressure() == 0, "the global wave still has a team fighting"
    kill(second_wave[2])
    wait(lambda: balance(second) == (25 + second_pool, 0) and math.isclose(pressure(), .04), "global second wave complete")
    assert data(first)["ledger"]["reward_generation"] == 0 and not data(first)["view"]["shop_open"]
    assert vitality(original) == 1, "a later clear strengthened a live wave"
    third_wave = add_wave(3, set(actors()))
    fresh = third_wave[1][0]
    wait(lambda: math.isclose(vitality(fresh), 1.04, abs_tol=.001), "new enemy receives the wave pressure")
    assert vitality(original) == 1
    kill(first_wave[1] + first_wave[2])
    wait(lambda: math.isclose(pressure(), .08), "out-of-order first wave clear")
    director(f"array<int> winners={{{heroes[1]}}}; ProgressionSurvivalClear(4,1,winners);")
    assert balance(first) == (50 + first_pool, 0)
    assert math.isclose(vitality(fresh), 1.04, abs_tol=.001)
    assert data(first)["ledger"]["completed_rounds"] == 2
    director("SessionSave(3);")
    wait(Path(host.writedir, "Data/SessionSaves/slot3.json").is_file, "save unfinished battery with banked wave XP")
    injure(heroes[2])
    kill(third_wave[1] + third_wave[2])
    wait(lambda: _probe(host, "int(phase)", "level", "level_specific") == "4", "battery victory camera")
    for seat in (first, second):
        view = data(seat)["view"]
        assert not view["shop_open"] and not view["reward_pending"]
    director("SessionSave(4);")
    win_save = Path(host.writedir, "Data/SessionSaves/slot4.json")
    wait(win_save.is_file, "manual save during the finishing camera")
    snapshot = json.loads(win_save.read_text())["session_mods"]
    assert snapshot["og-online-survival"]["data"]["phase"] == 4
    assert snapshot["og-online-progression"]["runtime"]["reward_generation"] == 0
    wait(lambda: data(first)["view"]["shop_open"] and math.isclose(pressure(), .2), "three waves and one battery clear")
    assert balance(first) == (175 + first_pool, 0) and balance(second) == (175 + second_pool, 0)
    assert all(data(seat)["view"]["xp_scale"]["world_remaining"] == 0 for seat in (first, second)), "rival players counted as wave XP"
    wait(lambda: len(panels()) == 2 and all(p.get("page") == "reward" for p in panels()), "free hands at battery intermission")
    ledger = data(first)["ledger"]
    assert ledger["reward_generation"] == 1 and ledger["completed_rounds"] == 3
    hand = next(account for account in ledger["players"] if account["id"] == second)["reward_hands"][0]
    assert "restore_health" in hand["order"], "the injured player's actual draw omitted recovery from its pool"
    bridge.call("key", key="1")
    wait(lambda: not data(first)["view"]["reward_pending"], "keyboard accepts battery upgrade")
    assert data(second)["view"]["reward_pending"]
    offered = [card["id"] for card in data(second)["view"]["reward_cards"]]
    card_id = "restore_health" if "restore_health" in offered else offered[0]
    before = balance(second)
    assert bridge.call("progression", actor=second, action="reward", key=card_id)["submitted"]
    assert balance(second) == before
    if card_id == "restore_health":
        assert fully_healed(heroes[2])
    for health, price in ((1.0, 1), (.4, 8), (.01, 14)):
        injure(heroes[2], health)
        recovery = next(c for c in data(second)["view"]["cards"] if c["id"] == "restore_health")
        assert (recovery["cost"], recovery["full_cost"]) == (price, 14)
        held = balance(second)[0]
        assert bridge.call("progression", actor=second, action="buy", key="restore_health")["submitted"]
        assert fully_healed(heroes[2]) and balance(second) == (held - price, 0)
        healthy = next(c for c in data(second)["view"]["cards"] if c["id"] == "restore_health")
        assert (healthy["cost"], healthy["full_cost"], healthy["available"]) == (0, 14, False)
        assert not bridge.call("progression", actor=second, action="buy", key="restore_health")["submitted"]

    assert bridge.call("progression", actor=second, action="ready")["submitted"]
    assert data(first)["view"]["shop_open"]
    assert bridge.call("progression", actor=first, action="ready")["submitted"]
    wait(lambda: not data(first)["view"]["shop_open"], "both seats finish their battery intermission")

    director("SessionLoad(3);")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "unfinished battery lobby")
    assert _probe(host, 'LobbySetModeSetting("progression_shop",0)') == "true"
    assert _probe(host, "LobbyStart()") == "true"
    _ready(host, SANDBOX_MAP)
    wait(lambda: len(actors()) == len(third_wave[1] + third_wave[2]), "restored final wave")
    assert balance(first) == (50 + first_pool, 0) and data(first)["ledger"]["completed_rounds"] == 2
    kill(actors())
    wait(lambda: len(panels()) == 2 and all(p.get("page") == "reward" for p in panels()), "free choices without a paid shop")
    assert not data(first)["view"]["shop_enabled"] and balance(first) == (175 + first_pool, 0)
    director("due = the_time;")
    for seat in (first, second):
        assert _probe(host, "battery", "level", "level_specific") == "4"
        card = data(seat)["view"]["reward_cards"][0]["id"]
        assert bridge.call("progression", actor=seat, action="reward", key=card)["submitted"]
    wait(lambda: not data(first)["view"]["shop_open"], "last free choice releases the break without ready votes")
    director("due = the_time;")
    wait(lambda: _probe(host, "battery", "level", "level_specific") == "5", "next battery after free-only intermission")
    # Reload after the last kill but before the free roll. The saved victory
    # must open its one shop and one set of free hands without replaying kills.
    director("SessionLoad(4);")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "victory save lobby")
    _code(host, "SetControllerPlayer(0);LobbyStart();")
    _ready(host, SANDBOX_MAP)
    wait(lambda: data(first)["view"]["shop_open"], "saved win finishes its camera and opens the shop")
    assert data(first)["ledger"]["reward_generation"] == 1
    assert balance(first) == (175 + first_pool, 0)
    for p in _rows(host):
        kill([p["avatar_id"]])
    wait(lambda: _probe(host, "int(phase)", "level", "level_specific") == "3", "a wipe in the open shop ends the run")
    for seat in (first, second):
        view = data(seat)["view"]
        assert not view["shop_open"] and not view["reward_pending"]
    wait(lambda: _probe(host, "level.showing_results", "level", "level_specific") == "true" and not panels(),
         "the split-screen scoreboard is visible with the progression HUD hidden")
    ctx.shot(host, "survival_split_results")
    bridge.call("pointer", x=300, y=120, buttons=4)
    bridge.call("pointer", x=300, y=120, buttons=0)
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"],
         "the interrupted intermission returns to the lobby")
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"wave_awards": 3, "battery_awards": 1, "teams": 2,
            "old_enemy_vitality": 1.0, "new_enemy_vitality": 1.04, "battery_pressure": pressure(),
            "reward_hands": 2}, "notes": []}
