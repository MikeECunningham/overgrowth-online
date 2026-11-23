"""Host-owned per-mode forms and challenge progression on two real peers."""

import math
import time
from pathlib import Path

from .campaign_scaling_suite import CAMPAIGN, LEVEL as CAMPAIGN_LEVEL, FACTORS, RENDER_KEYS
from .newcampaign_local_suite import _ready, _skip
from .scenarios import LEVEL, script_errors

KEYS = ("challenge_time", "challenge_round", "challenge_win", "challenge_death", "challenge_adaptive")
ARENA = "com-wolfire-arena"
ARENA_MAP = "Data/Levels/arenas/Magma_Arena.xml"
SANDBOX_MAP = "Data/Levels/" + LEVEL


def _code(inst, text, target="menu", context=None):
    return inst.bridge.call("exec", target=target, code=text, **({"context": context} if context else {}))


def _probe(inst, expression, target="menu", context=None):
    if target == "menu":
        _code(inst, 'SetConfigValueString("mode_setting_probe",""+(' + expression + '));SaveConfig();', target)
        config = dict(line.split(": ", 1) for line in (Path(inst.writedir) / "Data/config.txt").read_text().splitlines()
                      if ": " in line)
        return config["mode_setting_probe"]
    inst.bridge.call("level_messages", prefix="mode_setting_probe")
    _code(inst, 'level.SendMessage("mode_setting_probe "+(' + expression + '));', target, context)
    messages = inst.bridge.call("level_messages", prefix="mode_setting_probe")["messages"]
    assert messages, expression
    return messages[-1].split(" ", 1)[1]


def _settings(inst, target="menu"):
    return tuple(int(n) for n in _probe(inst, '""+' + '+"|"+'.join('LobbyGetModeSetting("' + key + '")' for key in KEYS), target).split("|"))


def _set(inst, **values):
    for name, value in values.items():
        assert _probe(inst, f'LobbySetModeSetting("challenge_{name}",{int(value)})') == "true", (name, value)


def _rows(inst):
    return inst.bridge.status()["online"]["players"]


def mode_settings_lobby(ctx):
    host = ctx.instance("mode_forms_host", width=1280, height=720,
                        keys={**RENDER_KEYS, "mp_port": "28004"})
    guest = ctx.instance("mode_forms_guest", width=800, height=600,
                         keys={**RENDER_KEYS, "mp_port": "28005"})
    metrics = {}
    wait = lambda predicate, label: host.bridge.wait(predicate, 35, step=0.1, what=label)
    _code(host, 'LobbyBeginHost();this_ui.SendCallback("multiplayer_lobby.as");')
    guest.bridge.call("connect", ip="127.0.0.1:28004")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "two pregame seats")
    for inst in (host, guest):
        inst.bridge.call("menu", script="multiplayer_lobby.as")
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
    wait(lambda: _probe(guest, 'LobbyGetMode()') == ARENA, "Arena selection replicated")
    assert _settings(host) == _settings(guest) == (0, 0, 0, 0, 0)
    _code(host, 'SetConfigValueBool("oo_arena_random",true);SaveConfig();')
    wait(lambda: _probe(guest, 'GetConfigValueBool("oo_arena_random")') == "true", "host Arena randomizer reaches guest UI")
    _code(guest, 'SetConfigValueBool("oo_arena_random",false);')
    assert _probe(guest, 'GetConfigValueBool("oo_arena_random")') == "true", "guest changed the host's Arena randomizer"
    assert _probe(host, 'GetConfigValueBool("oo_arena_random")') == "true"
    _set(host, time=2, round=1, win=3, death=2, adaptive=1)
    wait(lambda: _settings(guest) == (2, 1, 3, 2, 1), "all mode settings reach the guest")
    assert _probe(guest, 'LobbySetModeSetting("challenge_time",3)') == "false"
    assert _settings(host) == (2, 1, 3, 2, 1), "a non-host changed challenge settings"
    _code(guest, "small_panel=2;focus_lobby_form=true;")
    wait(lambda: any(w["name"] == "Mode settings" for w in guest.bridge.call("ui_state")["windows"]), "narrow mode settings tab")
    assert _probe(host, 'LobbyModeIsArena()') == "true"
    assert _probe(host, 'LobbyModeIsCampaign()') == "false"
    rects = {w["name"]: w["rect"] for w in host.bridge.call("ui_state")["windows"]
             if w["name"] in ("Mode and map", "Players", "Mode settings", "Hosting options")}
    assert len(rects) == 4, rects
    for name, (x1, y1, x2, y2) in rects.items():
        assert 0 <= x1 < x2 <= 1280 and 0 <= y1 < y2 <= 720, (name, rects)
    for index, (name, a) in enumerate(rects.items()):
        for other, b in list(rects.items())[index + 1:]:
            assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], (name, other, rects)
    metrics["wide_forms"] = rects
    metrics["narrow_forms"] = guest.bridge.call("ui_state")["windows"]
    for window in metrics["narrow_forms"]:
        if window["name"] in ("Lobby", "Mode settings", "##lobby_sections"):
            x1, y1, x2, y2 = window["rect"]
            assert 0 <= x1 < x2 <= 800 and 0 <= y1 < y2 <= 600, window
    ctx.shot(host, "arena_mode_settings_wide")
    ctx.shot(guest, "arena_mode_settings_narrow")
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","{CAMPAIGN_LEVEL}",false);')
    wait(lambda: _probe(guest, 'LobbyGetMode()') == CAMPAIGN, "campaign selection replicated")
    assert _settings(host) == _settings(guest) == (0, 0, 0, 0, 0), "Arena preferences leaked into campaign"
    _set(host, time=1, round=3, win=2, death=1)
    wait(lambda: _settings(guest) == (1, 3, 2, 1, 0), "campaign settings reach the guest")
    assert _probe(host, 'LobbyModeIsCampaign()') == "true"
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
    wait(lambda: _settings(guest) == (2, 1, 3, 2, 1), "returning to Arena restores its preferences")
    _set(host, win=0)
    wait(lambda: _settings(guest)[2:] == (0, 2, 0), "disabling encounter wins clears adaptive mode")
    assert _probe(host, 'LobbySetModeSetting("challenge_adaptive",1)') == "false"
    _code(host, f'LobbySetSelection("sandbox","","{SANDBOX_MAP}",false);')
    wait(lambda: _probe(guest, 'LobbyGetMode()') == "sandbox", "sandbox selection replicated")
    assert _settings(host) == (0, 0, 0, 0, 0)
    assert _probe(host, 'LobbySetModeSetting("challenge_round",2)') == "false"
    _set(host, time=3, win=1, death=2)
    _code(host, 'LobbySetSelection("build","","Data/Levels/nothing.xml",true);')
    wait(lambda: _probe(guest, 'LobbyGetBuildMode()') == "true", "Build selection replicated")
    assert _probe(host, 'LobbySetModeSetting("challenge_time",2)') == "false"
    ctx.shot(guest, "build_mode_settings")
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:8])
    return {"passed": True, "metrics": metrics, "notes": []}


def mode_challenge(ctx):
    host = ctx.instance("challenge_host", width=640, height=360,
                        keys={**RENDER_KEYS, "mp_port": "28014", "player_invincible": "true",
                              "campaign_mp_scaling": "false", "campaign_mp_nightmare": "false", "oo_arena_random": "false"})
    guest = ctx.instance("challenge_guest", width=640, height=360,
                         keys={**RENDER_KEYS, "mp_port": "28015", "player_invincible": "true"})
    metrics = {}

    def wait(predicate, label, seconds=35):
        return host.bridge.wait(predicate, seconds, step=0.1, what=label)

    def pressure(inst=host):
        return float(_probe(inst, '""+Online_ChallengePressure()', "level"))

    def actor(player, inst=host):
        row = next(p for p in _rows(inst) if p["id"] == player)
        return next(c for c in inst.bridge.state() if c["id"] == row["avatar_id"])

    def begin(key):
        _code(host, f'Online_ChallengeEncounterBegin("{key}");', "level")

    def win(key):
        _code(host, f'Online_ChallengeEncounterWin("{key}");', "level")

    def expected(value, label):
        wait(lambda: math.isclose(pressure(), value, abs_tol=0.0002), label)
        wait(lambda: math.isclose(pressure(guest), value, abs_tol=0.0002), label + " reaches guest", 12)

    def knock(player, inst):
        _code(inst, 'SetKnockedOut(_dead);', actor(player, inst)["id"])
        wait(lambda: actor(player)["knocked_out"] != 0, "owner death reaches host")
        time.sleep(0.2)

    def recover(player, inst):
        _code(inst, 'Recover();', actor(player, inst)["id"])
        wait(lambda: actor(player)["knocked_out"] == 0, "owner recovery reaches host")
        time.sleep(0.2)

    def end():
        _code(host, 'LobbyEndGame();', "level")
        wait(lambda: all(i.bridge.status()["online"]["lobby_waiting"] and not i.bridge.status()["level_loaded"]
                         for i in (host, guest)), "return to shared lobby")

    def start(sandbox=True):
        _code(host, 'LobbyStart();')
        _ready(host)
        _ready(guest)
        if sandbox:
            roster = {p["avatar_id"] for p in _rows(host)}
            for c in host.bridge.state():
                if c["id"] not in roster:
                    _code(host, f'ReadObjectFromID({c["id"]}).SetEnabled(false);', "level")
            for p in _rows(host):
                if p.get("bot"):
                    _code(host, 'combat_allowed=false;chase_allowed=false;hostile=false;always_unaware=true;'
                          'invincible=true;this_mo.static_char=true;', p["avatar_id"])

    def freeze():
        for inst in (host, guest):
            _code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                  'if(c.controlled&&!c.network_proxy){c.static_char=true;c.Execute("invincible=true;");}}', "level")

    _code(host, f'LobbyBeginHost();LobbyAddBot();LobbyAddBot();LobbySetSelection("sandbox","","{SANDBOX_MAP}",false);')
    guest.bridge.call("connect", ip="127.0.0.1:28014")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 4, "challenge party and enemy bot")
    humans = [p for p in _rows(host) if not p.get("bot")]
    host_human = next(p["id"] for p in humans if p["peer_id"] == 255)
    guest_human = next(p["id"] for p in humans if p["peer_id"] != 255)
    friendly_bot, enemy_bot = sorted(p["id"] for p in _rows(host) if p.get("bot"))
    for p in _rows(host):
        _code(host, f'LobbySetPlayerTeam({p["id"]},1);')
    _code(host, f'LobbySetPlayerTeam({enemy_bot},2);')
    _set(host, time=0, win=2, death=2)
    start()
    freeze()
    roster = {p["avatar_id"] for p in _rows(host)}
    for c in host.bridge.state():
        if c["id"] not in roster:
            _code(host, f'ReadObjectFromID({c["id"]}).SetEnabled(false);', "level")
    base = actor(host_human)["pos"]
    enemy = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                             pos=[base[0] + 2, base[1] + 0.2, base[2] + 2], teams="challenge_enemy")["object_id"]
    _code(host, 'this_mo.static_char=true;combat_allowed=false;hostile=false;invincible=true;', enemy)
    begin("regular_a")
    win("regular_a")
    expected(0.04, "one standard encounter award")
    def enemy_factors(inst):
        c = next(c for c in inst.bridge.state() if c["common_id"] == enemy)
        return tuple(c["combat_stats"].get(k, -1) for k in FACTORS)
    wait(lambda: all(math.isclose(enemy_factors(i)[0], 1.04, abs_tol=1e-5) for i in (host, guest)), "NPC health multiplier reaches both peers")
    wait(lambda: all(math.isclose(actor(enemy_bot, i)["combat_stats"][FACTORS[0]], 1.04, abs_tol=1e-5)
                     for i in (host, guest)), "enemy roster bot receives progression on both peers")
    assert actor(enemy_bot)["combat_stats"][FACTORS[3]] == 0, "enemy roster bot received Nightmare tuning"
    assert all(tuple(actor(p)["combat_stats"].get(k, 1 if k != FACTORS[3] else 0) for k in FACTORS) == (1, 1, 1, 0)
               for p in (host_human, guest_human, friendly_bot)), "challenge buffed party members"
    win("regular_a")
    expected(0.04, "duplicate encounter callbacks are ignored")
    begin("regular_b")
    knock(guest_human, guest)
    expected(0.09, "a human defeat increases challenge pressure")
    recover(guest_human, guest)
    win("regular_b")
    expected(0.13, "encounter wins add to the selected defeat pressure")
    knock(friendly_bot, host)
    expected(0.13, "friendly bot defeats add no human defeat pressure")
    recover(friendly_bot, host)
    # Generic modes infer an encounter from actual combat, without authored
    # Begin/Win calls. Its last defeated foe must grant one automatic award.
    _code(host, f'this_mo.static_char=true;SetTargetID({enemy});SetState(_attack_state);', actor(host_human)["id"])
    wait(lambda: actor(host_human)["state"] == 2 and actor(host_human)["ai_target_id"] == enemy,
         "held generic attack cue")
    wait(lambda: enemy in host.bridge.status()["online"]["challenge"]["foes"], "generic combat captures its target", 10)
    metrics["generic_before_ko"] = host.bridge.status()["online"]["challenge"]
    _code(host, 'SetKnockedOut(_dead);', enemy)
    _code(host, 'SetTargetID(-1);SetState(_movement_state);', actor(host_human)["id"])
    expected(0.17, "generic combat capture awards the final foe automatically")
    metrics["regular"] = {"pressure": pressure(), "enemy_factors": enemy_factors(host)}
    end()
    assert _settings(host)[2:4] == (2, 2), "lobby cycle lost the selected mode preferences"
    _set(host, adaptive=1, death=3)
    start()
    freeze()
    expected(0, "a new game starts a fresh challenge run")
    begin("adaptive_a")
    knock(guest_human, guest)
    expected(0, "adaptive mode discounts future wins instead of adding defeat pressure")
    recover(guest_human, guest)
    win("adaptive_a")
    expected(0.03, "one encounter death discounts its award by twenty-five percent")
    knock(guest_human, guest)
    expected(0.03, "later deaths do not recut a credited win")
    recover(guest_human, guest)
    win("adaptive_a")
    expected(0.03, "repeated adaptive win stays credited exactly once")
    begin("adaptive_b")
    win("adaptive_b")
    expected(0.07, "a clean encounter earns its full award")
    metrics["adaptive"] = pressure()
    end()
    _set(host, time=2, win=0, death=0)
    start()
    freeze()
    wait(lambda: pressure() > 0.0005, "active gameplay advances time pressure", 12)
    _code(host, 'LobbySetPaused(true);', "level")
    wait(lambda: host.bridge.status()["paused"], "host pause")
    paused = pressure()
    time.sleep(2)
    assert math.isclose(pressure(), paused, abs_tol=0.00005), ("paused time increased challenge", paused, pressure())
    _code(host, 'LobbySetPaused(false);', "level")
    wait(lambda: pressure() > paused + 0.0002, "active clock resumes after pause")
    metrics["active_clock"] = {"paused": paused, "resumed": pressure()}
    end()
    _code(host, f'LobbyRemovePlayer({enemy_bot});')
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 3, "remove the generic-mode opponent before Arena")

    # Resolve a real online Arena round/match, exercising the new AS hooks.
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);LobbySetForceTeams(true);'
          f'LobbySetPlayerTeam({host_human},1);LobbySetPlayerTeam({friendly_bot},1);LobbySetPlayerTeam({guest_human},2);')
    _set(host, time=0, round=2, win=2, death=0)
    start(False)
    wait(lambda: _probe(host, '""+int(phase)', "level", "level_specific") == "2", "actual Arena fighting phase", 45)
    _code(host, 'max_points=1;for(uint i=0;i<bots.length();++i)'
          'ReadCharacterID(bots[i]).ReceiveScriptMessage("set_combat_allowed false");', "level", "level_specific")
    freeze()
    knock(guest_human, guest)
    wait(lambda: pressure() >= 0.1199, "real Arena round win and completed match award", 25)
    expected(0.12, "Arena completion and win each apply exactly once")
    metrics["arena"] = pressure()
    end()

    # Hold Ice Cliff's real objective on a disposable enemy and defeat it via
    # the game's normal character event; a retry must not credit the same goal.
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","{CAMPAIGN_LEVEL}",false);'
          'LobbySetForceTeams(false);SetConfigValueBool("campaign_mp_scaling",false);')
    _set(host, time=0, round=2, win=2, death=0)
    start(False)
    _skip(host)
    freeze()
    base = actor(host_human)["pos"]
    foe = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                           pos=[base[0] + 1, base[1] + 0.2, base[2] + 1], teams="b")["object_id"]
    _code(host, 'this_mo.static_char=true;combat_allowed=false;invincible=true;', foe)
    _code(host, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);Object@ o=ReadObjectFromID(h.GetID());'
          'if(h.HasVar("progress")&&o.GetScriptParams().HasParam("goal_0")){'
          f'o.GetScriptParams().SetString("goal_0","defeat {foe}");'
          'o.GetScriptParams().SetString("goal_0_pre","");o.GetScriptParams().SetString("goal_0_post","");h.SetIntVar("progress",0);}}', "level")
    time.sleep(0.7)
    assert pressure() == 0, "an undefeated authored objective earned a challenge win"
    _code(host, 'SetKnockedOut(_dead);', foe)
    expected(0.04, "real campaign defeat objective awards a win")
    _code(host, 'level.SendMessage("reset");', "level")
    _skip(host)
    wait(lambda: next(c for c in host.bridge.state() if c["id"] == foe)["knocked_out"] == 0, "retry revives the objective fixture")
    _code(host, 'SetKnockedOut(_dead);', foe)
    expected(0.04, "retrying the same campaign goal does not duplicate its award")
    _code(host, 'SendGlobalMessage("levelwin");', "level")
    _ready(host)
    _ready(guest)
    wait(lambda: pressure() >= 0.1199, "actual campaign level completion award", 35)
    expected(0.12, "campaign goal and level completion each apply once")
    metrics["campaign"] = pressure()
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:8])
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
    return {"passed": True, "metrics": metrics, "notes": []}


def lobby_tooltips(ctx):
    inst = ctx.instance("lobby_tooltips", width=1280, height=720,
                        keys={**RENDER_KEYS, "mp_port": "28024"})
    bridge = inst.bridge
    wait = lambda predicate, label: bridge.wait(predicate, 20, step=0.1, what=label)
    def tooltip():
        return next((w for w in bridge.call("ui_state")["windows"] if w["name"].startswith("##Tooltip")), None)
    def pad(button):
        for down in (True, False):
            bridge.call("virtual_pad", action="button", slot=0, button=button, down=down)
            time.sleep(0.1)
    bridge.call("menu", script="multiplayer_menu.as")
    time.sleep(0.25)
    bridge.call("virtual_pad", action="attach", slot=0)
    time.sleep(0.15)
    pad(0)  # The first controller chooses Host before a lobby exists.
    wait(lambda: bridge.status()["online"]["lobby_waiting"], "controller opens Host lobby")
    bridge.call("pointer", x=0, y=0, buttons=0)
    wait(lambda: bridge.call("ui_state")["nav_id"] != 0 and tooltip(), "controller-focused lobby description")
    previous_nav = bridge.call("ui_state")["nav_id"]
    # X + shoulder is the game's controller gesture for changing form/window.
    bridge.call("virtual_pad", action="button", slot=0, button=2, down=True)
    time.sleep(0.1)
    pad(10)
    bridge.call("virtual_pad", action="button", slot=0, button=2, down=False)
    time.sleep(0.2)
    wait(lambda: bridge.call("ui_state")["nav_id"] != previous_nav and tooltip(), "controller moves focus and sees its description")
    assert len(_rows(inst)) == 1, "tooltip navigation unexpectedly joined a player"
    roles = [(p["id"], p["host"], p["leader"], p["name"]) for p in _rows(inst)]
    ctx.shot(inst, "controller_focused_hint")

    # Hovering another control while a name draft is active must only show
    # its description, preserving both the text and text input ownership.
    _code(inst, f'editing_name={roles[0][0]};name_buffer="Unfinished draft";name_keyboard_open=false;')
    wait(lambda: bridge.call("ui_state")["want_text_input"], "name editor captures text")
    bridge.call("text", text=" Keep")
    time.sleep(0.15)
    draft = _probe(inst, 'name_buffer')
    active = bridge.call("ui_state")["active_id"]
    hosting = next(w["rect"] for w in bridge.call("ui_state")["windows"] if w["name"] == "Hosting options")
    bridge.call("pointer", x=int((hosting[0] + hosting[2]) * 0.5), y=int(hosting[1] + 88), buttons=0)
    wait(lambda: tooltip(), "mouse hover description")
    assert _probe(inst, 'name_buffer') == draft, "tooltip hover changed the live name draft"
    assert bridge.call("ui_state")["active_id"] == active, "tooltip hover stole text input focus"
    assert [(p["id"], p["host"], p["leader"], p["name"]) for p in _rows(inst)] == roles
    ctx.shot(inst, "mouse_hover_preserves_draft")
    _code(inst, 'editing_name=-1;')
    bridge.call("pointer", enabled=False)
    bridge.call("virtual_pad", action="clear")
    assert not script_errors(inst), script_errors(inst)[:8]
    return {"passed": True, "metrics": {"controller_hint": True, "mouse_hint": True, "draft": draft}, "notes": []}
