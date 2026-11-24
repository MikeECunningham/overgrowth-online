"""Host-owned per-mode forms and challenge progression on two real peers."""

import math
import time
from pathlib import Path

from .campaign_scaling_suite import CAMPAIGN, LEVEL as CAMPAIGN_LEVEL, FACTORS, RENDER_KEYS
from .newcampaign_local_suite import _ready, _skip
from .scenarios import LEVEL, script_errors

KEYS = ("challenge_time", "challenge_round", "challenge_performance", "challenge_performance_rate")
ARENA = "com-wolfire-arena"
ARENA_MAP = "Data/Levels/arenas/Magma_Arena.xml"
SANDBOX_MAP = "Data/Levels/" + LEVEL


def _code(inst, text, target="menu", context=None):
    return inst.bridge.call("exec", target=target, code=text, **({"context": context} if context else {}))


def _probe(inst, expression, target="menu", context=None):
    if target == "menu":
        _code(inst, 'SetConfigValueString("test_mode_setting_probe",""+(' + expression + '));SaveConfig();', target)
        config = dict(line.split(": ", 1) for line in (Path(inst.writedir) / "Data/config.txt").read_text().splitlines()
                      if ": " in line)
        # An initially empty result may not dirty/save a new config key.
        return config.get("test_mode_setting_probe", "")
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
                        keys={**RENDER_KEYS, "mp_port": "28004", "difficulty_scaling": "true"})
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
    assert _settings(host) == _settings(guest) == (2, 2, 0, 2)
    _code(host, 'SetConfigValueBool("oo_arena_random",true);SaveConfig();')
    wait(lambda: _probe(guest, 'GetConfigValueBool("oo_arena_random")') == "true", "host Arena randomizer reaches guest UI")
    _code(guest, 'SetConfigValueBool("oo_arena_random",false);')
    assert _probe(guest, 'GetConfigValueBool("oo_arena_random")') == "true", "guest changed the host's Arena randomizer"
    assert _probe(host, 'GetConfigValueBool("oo_arena_random")') == "true"
    assert _probe(host,'GetConfigValueInt("campaign_ai_scaling_rate")') == "2"
    _code(host,'SetConfigValueInt("campaign_ai_scaling_rate",4);')
    wait(lambda:_probe(guest,'GetConfigValueInt("campaign_ai_scaling_rate")') == "4","Nightmare AI rate reaches guest")
    _code(guest,'SetConfigValueInt("campaign_ai_scaling_rate",1);')
    assert _probe(guest,'GetConfigValueInt("campaign_ai_scaling_rate")') == "4","guest changed host AI strength"
    wait(lambda: _probe(guest, 'GetConfigValueBool("difficulty_scaling")') == "true", "Scaling master mode reaches the guest")
    _code(guest, 'SetConfigValueBool("difficulty_scaling",false);')
    assert _probe(guest, 'GetConfigValueBool("difficulty_scaling")') == "true", "guest changed global difficulty mode"
    _code(host, 'SetConfigValueBool("armor_localized",false);')
    wait(lambda: _probe(guest, 'GetConfigValueBool("armor_localized")') == "false", "whole-body armor choice reaches the guest")
    _code(guest, 'SetConfigValueBool("armor_localized",true);')
    assert _probe(guest, 'GetConfigValueBool("armor_localized")') == "false", "guest changed armor hit coverage"
    _code(host, 'SetConfigValueBool("armor_localized",true);')
    wait(lambda: _probe(guest, 'GetConfigValueBool("armor_localized")') == "true", "localized armor choice reaches the guest")
    _set(host,time=4,round=4,performance=3,performance_rate=4)
    wait(lambda:_settings(guest)==(4,4,3,4),"all Nightmare rates reach guest")
    assert _probe(host,'LobbySetModeSetting("challenge_time",5)') == "false"
    assert _probe(host,'LobbySetModeSetting("challenge_performance",4)') == "false"
    _set(host, time=2, round=1, performance=3, performance_rate=2)
    wait(lambda: _settings(guest) == (2, 1, 3, 2), "all mode settings reach the guest")
    assert _probe(guest, 'LobbySetModeSetting("challenge_time",3)') == "false"
    assert _settings(host) == (2, 1, 3, 2), "a non-host changed challenge settings"
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
    for inst in (host, guest):
        _code(inst, 'lobby_difficulty_open=true;lobby_difficulty_focus=true;')
    wait(lambda: all(any(w["name"] == "Scaling difficulty" for w in i.bridge.call("ui_state")["windows"])
                     for i in (host, guest)), "dedicated Scaling submenu opens for host and read-only guest")
    for inst, width, height in ((host, 1280, 720), (guest, 800, 600)):
        modal = next(w for w in inst.bridge.call("ui_state")["windows"] if w["name"] == "Scaling difficulty")
        x1, y1, x2, y2 = modal["rect"]
        assert 0 <= x1 < x2 <= width and 0 <= y1 < y2 <= height, modal
    assert _probe(host, '""+GetConfigValueOptions("difficulty_preset").length()') == "3"
    ctx.shot(host, "arena_mode_settings_wide")
    ctx.shot(guest, "arena_mode_settings_narrow")
    # The personal settings menu uses compact difficulty cards too. Click
    # each real element and use its normal callback, rather than sending
    # its message directly.
    host.bridge.call("menu",script="main_menu_settings.as")
    _code(host,'SwitchSettingsScreen("Difficulty");')
    for rate in (1,4):
        _code(host,'for(uint i=0;i<controller_items.length();++i){IMElement@ e=controller_items[i];'
              f'if(e.getName()=="rate_campaign_ai_scaling_rate_{rate}")'+'{vec2 p=e.getScreenPosition();'
              'SetConfigValueString("test_rate_box",""+(p.x+e.getSizeX()*screenMetrics.GUItoScreenXScale*.5f)+"|"+'
              '(p.y+e.getSizeY()*screenMetrics.GUItoScreenYScale*.5f));break;}}')
        x,y=map(float,_probe(host,'GetConfigValueString("test_rate_box")').split("|"))
        for buttons in (0,1,0):
            host.bridge.call("pointer",x=int(x),y=int(y),buttons=buttons);time.sleep(.15)
        wait(lambda:_probe(host,'GetConfigValueInt("campaign_ai_scaling_rate")') == str(rate),"mouse selects AI rate card")
        wait(lambda:_probe(guest,'GetConfigValueInt("campaign_ai_scaling_rate")') == str(rate),"card rate reaches read-only guest")
    host.bridge.call("pointer",enabled=False)
    ctx.shot(host,"difficulty_rate_cards_settings")
    host.bridge.call("menu",script="multiplayer_lobby.as")
    for inst in (host, guest):
        _code(inst, 'lobby_difficulty_open=false;')
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","{CAMPAIGN_LEVEL}",false);')
    wait(lambda: _probe(guest, 'LobbyGetMode()') == CAMPAIGN, "campaign selection replicated")
    assert _settings(host) == _settings(guest) == (2, 2, 0, 2), "Arena preferences leaked into campaign"
    _set(host, time=1, round=3, performance=2, performance_rate=1)
    wait(lambda: _settings(guest) == (1, 3, 2, 1), "campaign settings reach the guest")
    assert _probe(host, 'LobbyModeIsCampaign()') == "true"
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
    wait(lambda: _settings(guest) == (2, 1, 3, 2), "returning to Arena restores its preferences")
    _set(host, performance=0)
    wait(lambda: _settings(guest)[2:] == (0, 2), "disabling performance preserves its selected rate")
    assert _probe(host, 'LobbySetModeSetting("challenge_adaptive",1)') == "false"
    assert _probe(host, 'LobbySetModeSetting("challenge_death",2)') == "false"
    assert _probe(host, 'LobbySetModeSetting("challenge_win",2)') == "false"
    _code(host, f'LobbySetSelection("sandbox","","{SANDBOX_MAP}",false);')
    wait(lambda: _probe(guest, 'LobbyGetMode()') == "sandbox", "sandbox selection replicated")
    assert _settings(host) == (2, 2, 0, 2)
    assert _probe(host, 'LobbySetModeSetting("challenge_round",2)') == "false"
    _set(host, time=3, performance=1, performance_rate=2)
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
                              "difficulty_scaling": "true", "campaign_mp_scaling": "false", "campaign_ai_scaling": "false",
                              "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false", "oo_arena_random": "false"})
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
    _set(host, time=0, performance=1, performance_rate=2)
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
    assert _probe(host, 'LobbySetModeSetting("challenge_performance_rate",1)', "level") == "true"
    assert _probe(guest, 'LobbySetModeSetting("challenge_performance_rate",3)', "level") == "false"
    assert _probe(host, 'LobbySetModeSetting("challenge_performance_rate",2)', "level") == "true"
    begin("regular_a")
    win("regular_a")
    expected(0.04, "one standard encounter award")
    def enemy_factors(inst):
        c = next(c for c in inst.bridge.state() if c["common_id"] == enemy)
        return tuple(c["combat_stats"].get(k, -1) for k in FACTORS)
    time.sleep(.8)
    assert all(enemy_factors(i)[0] == 1 for i in (host, guest)), "a victory buffed living enemies mid-fight"
    _code(host, 'Online_CaptureEnemyDifficulty();', "level")
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
    expected(0.04, "a partial human defeat adds no performance pressure")
    recover(guest_human, guest)
    win("regular_b")
    expected(0.08, "On win adds the selected encounter increase")
    knock(friendly_bot, host)
    expected(0.08, "friendly bot defeats add no performance pressure")
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
    expected(0.12, "generic combat capture awards the final foe automatically")
    metrics["regular"] = {"pressure": pressure(), "enemy_factors": enemy_factors(host)}
    end()
    assert _settings(host)[2:4] == (1, 2), "lobby cycle lost the selected mode preferences"
    _set(host, performance=2, performance_rate=2)
    start()
    freeze()
    expected(0, "a new game starts a fresh challenge run")
    begin("loss_a")
    knock(guest_human, guest)
    expected(0, "one surviving human vetoes On loss")
    knock(host_human, host)
    expected(0.05, "one full human wipe increases On loss pressure despite a surviving bot")
    first_wipe = host.bridge.status()["online"]["challenge"]
    assert first_wipe["wipe_latched"] and len(first_wipe["wipes"]) == 1, first_wipe
    time.sleep(0.5)
    expected(0.05, "continued death and KO notifications do not duplicate a wipe")
    recover(host_human, host)
    knock(host_human, host)
    expected(0.05, "a partial recovery cannot rearm the same failed attempt")
    recover(host_human, host)
    recover(guest_human, guest)
    time.sleep(0.5)
    win("loss_a")
    expected(0.05, "On loss does not also reward victories")
    metrics["on_loss"] = host.bridge.status()["online"]["challenge"]
    end()
    _set(host, performance=3, performance_rate=2)
    start()
    freeze()
    begin("forced_a")
    knock(guest_human, guest)
    expected(0, "Forced50 ignores individual teammate defeats")
    knock(host_human, host)
    expected(-0.025, "Forced50 immediately eases this encounter's next attempt")
    _code(host, 'level.SendMessage("reset");', "level")
    # The applied enemy state after a retry must cross below
    # baseline on both peers. Humans and friendly lobby bots remain unchanged.
    wait(lambda: all(math.isclose(actor(enemy_bot, i)["combat_stats"][FACTORS[0]], 0.975, abs_tol=1e-5)
                     for i in (host, guest)), "signed retry relief reaches enemy bot on both peers")
    guest.bridge.call("disconnect")
    departure_states = []
    def left_world():
        state = guest.bridge.status()
        departure_states.append({"loaded": state["level_loaded"], "active": state["online"]["active"],
                                 "hosting": state["online"]["hosting"]})
        assert not state["online"]["hosting"], "Leave automatically adopted the old world as a private host"
        return not state["online"]["active"] and not state["level_loaded"]
    wait(left_world, "guest Leave finishes without private re-adoption")
    wait(lambda: len(_rows(host)) == 3, "old peer leaves while relief remains")
    metrics["disconnect_transition"] = departure_states
    guest.bridge.call("connect", ip="127.0.0.1:28014")
    _ready(guest)
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 4, "late peer receives the active run")
    guest_human = next(p["id"] for p in _rows(host) if not p.get("bot") and p["peer_id"] != 255)
    _code(host, f'LobbySetPlayerTeam({guest_human},1);', "level")
    freeze()
    expected(-0.025, "late peer receives signed current relief and one credited wipe")
    assert guest.bridge.status()["online"]["challenge"]["wipes"] == host.bridge.status()["online"]["challenge"]["wipes"]
    metrics["late_join_relief"] = guest.bridge.status()["online"]["challenge"]
    recover(host_human, host)
    recover(guest_human, guest)
    time.sleep(0.5)
    begin("forced_a")  # retrying the logical encounter preserves its relief
    expected(-0.025, "retry does not clear the current encounter's easing")
    knock(host_human, host)
    knock(guest_human, guest)
    expected(-0.05, "a second full wipe applies one further small retry ease")
    recover(host_human, host)
    recover(guest_human, guest)
    time.sleep(0.5)
    win("forced_a")
    expected(0.08, "Forced50 win removes local easing and increases general pressure")
    win("forced_a")
    expected(0.08, "a repeated victory cannot farm general pressure")
    begin("forced_b")
    expected(0.08, "the next encounter inherits no relief from previous failures")
    metrics["forced50"] = host.bridge.status()["online"]["challenge"]
    end()
    _code(host, 'SetConfigValueBool("difficulty_scaling",false);')
    _set(host, time=2, performance=3, performance_rate=2)
    start()
    freeze()
    begin("traditional_a")
    win("traditional_a")
    begin("traditional_b")
    knock(guest_human, guest)
    knock(host_human, host)
    expected(0, "Traditional mode suppresses configured time and performance scaling")
    assert actor(enemy_bot)["combat_stats"][FACTORS[0]] == 1
    wait(lambda: len(host.bridge.status()["online"]["challenge"]["wipes"]) == 1,
         "Traditional observes the failed attempt without applying pressure")
    _code(host, 'SetConfigValueBool("difficulty_scaling",true);', "level")
    expected(0, "enabling Scaling while already wiped never reprices an earlier defeat")
    recover(host_human, host)
    recover(guest_human, guest)
    metrics["traditional"] = host.bridge.status()["online"]["challenge"]
    end()
    _code(host, 'SetConfigValueBool("difficulty_scaling",true);')
    _set(host, time=2, performance=0)
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
    _set(host, time=0, round=2, performance=1, performance_rate=2)
    start(False)
    wait(lambda: _probe(host, '""+int(phase)', "level", "level_specific") == "2", "actual Arena fighting phase", 45)
    _code(host, 'max_points=1;for(uint i=0;i<bots.length();++i)'
          'ReadCharacterID(bots[i]).ReceiveScriptMessage("set_combat_allowed false");', "level", "level_specific")
    freeze()
    knock(guest_human, guest)
    wait(lambda: pressure() >= 0.1199, "real Arena round win and completed match award", 25)
    expected(0.12, "Arena completion and win each apply exactly once")
    wait(lambda: math.isclose(host.bridge.status()["online"]["challenge"]["enemy_difficulty"]["pressure"], .12, abs_tol=.0002),
         "new Arena match captures the completed match award", 35)
    metrics["arena"] = pressure()
    end()

    # Hold Ice Cliff's real objective on a disposable enemy and defeat it via
    # the game's normal character event; a retry must not credit the same goal.
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","{CAMPAIGN_LEVEL}",false);'
          'LobbySetForceTeams(false);SetConfigValueBool("campaign_mp_scaling",false);')
    _set(host, time=0, round=2, performance=1, performance_rate=2)
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
    end()
    before_transfer = host.bridge.status()["online"]["challenge"]
    assert _probe(host, f'LobbyPromoteHost({guest_human})') == "true"
    wait(lambda: all(not i.bridge.status()["online"]["migration_busy"] and
                     i.bridge.status()["online"]["decision_host"] == guest_human for i in (host, guest)),
         "host migration restores the authoritative performance state", 70)
    assert guest.bridge.status()["online"]["hosting"] and host.bridge.status()["online"]["client"]
    migrated = guest.bridge.status()["online"]["challenge"]
    for key in ("version", "pressure", "seconds", "wipes", "wipe_serial", "wipe_latched", "encounters", "completions", "modes"):
        assert migrated[key] == before_transfer[key], (key, before_transfer[key], migrated[key])
    metrics["migrated_progression"] = migrated

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
    def hints():
        return tuple(_probe(inst, 'lobby_hover_hint+"|"+lobby_focus_hint+"|"+lobby_visible_hint').split("|"))
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
    _code(inst, f'editing_name={roles[0][0]};name_buffer="";name_keyboard_open=false;')
    wait(lambda: bridge.call("ui_state")["want_text_input"], "name editor captures text")
    bridge.call("text", text="NameDraft")
    wait(lambda: _probe(inst, 'name_buffer') == "NameDraft", "name editor accepts the draft")
    draft = _probe(inst, 'name_buffer')
    name_ui = bridge.call("ui_state")
    active = name_ui["active_id"]
    name_field = name_ui["nav_rect"]
    name_hint = "Edit the player's display name. Enter saves."
    start_hint = "Start the selected mode and map with this roster. Resets difficulty progression."
    players = next(w["rect"] for w in bridge.call("ui_state")["windows"] if w["name"] == "Players")
    # Session actions live in the fixed Players footer. Find the actual Start
    # tooltip in its bounded footer instead of tying the test to font metrics.
    for y in range(int(players[3] - 190), int(players[3] - 20), 12):
        bridge.call("pointer", x=int((players[0] + players[2]) * 0.5), y=y, buttons=0)
        time.sleep(0.04)
        if hints()[0] == start_hint:
            break
    assert hints()[0] == start_hint, ("Start footer tooltip missing", hints(), players)
    # The name field is drawn after Hosting options and remains focused. Its
    # candidate must not append to or replace the hovered Start description.
    wait(lambda: hints() == (start_hint, name_hint, start_hint) and tooltip(),
         "Start hover takes precedence over the focused name field")
    assert _probe(inst, 'name_buffer') == draft, "tooltip hover changed the live name draft"
    assert bridge.call("ui_state")["active_id"] == active, "tooltip hover stole text input focus"
    bridge.call("pointer", x=int((name_field[0] + name_field[2]) * 0.5),
                y=int((name_field[1] + name_field[3]) * 0.5), buttons=0)
    wait(lambda: hints() == (name_hint, "", name_hint) and tooltip(),
         "second hover replaces the previous description")
    assert _probe(inst, 'name_buffer') == draft, "second tooltip hover changed the live name draft"
    assert bridge.call("ui_state")["active_id"] == active, "second tooltip hover stole text input focus"
    assert [(p["id"], p["host"], p["leader"], p["name"]) for p in _rows(inst)] == roles
    ctx.shot(inst, "mouse_hover_preserves_draft")
    _code(inst, 'editing_name=-1;')
    bridge.call("pointer", enabled=False)
    bridge.call("virtual_pad", action="clear")
    assert not script_errors(inst), script_errors(inst)[:8]
    return {"passed": True, "metrics": {"controller_hint": True, "mouse_hint": True,
            "hover_precedes_focus": True, "hover_replaced": True, "draft": draft}, "notes": []}


def performance_solo(ctx):
    """Scaling activates the same private authority for a direct solo launch."""
    inst = ctx.instance("performance_solo", width=960, height=540,
                        keys={**RENDER_KEYS, "mp_port": "28034", "difficulty_scaling": "true",
                              "campaign_mp_scaling": "false", "campaign_ai_scaling": "false",
                              "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false",
                              "player_invincible": "true"})
    bridge = inst.bridge
    wait = lambda predicate, label: bridge.wait(predicate, 30, step=0.08, what=label)
    assert not bridge.status()["online"]["active"]
    bridge.call("load_level", path=SANDBOX_MAP)
    _ready(inst)
    wait(lambda: bridge.status()["online"]["hosting"] and len(_rows(inst)) == 1,
         "direct solo Scaling adopts a private local session")
    _code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
          'c.static_char=true;c.Execute("combat_allowed=false;hostile=false;invincible=true;");}', "level")
    assert _probe(inst, 'LobbySetModeSetting("challenge_time",0)', "level") == "true"
    assert _probe(inst, 'LobbySetModeSetting("challenge_performance",3)', "level") == "true"
    assert _probe(inst, 'LobbySetModeSetting("challenge_performance_rate",2)', "level") == "true"
    _code(inst, 'Online_ChallengeEncounterBegin("solo_retry");', "level")
    human = _rows(inst)[0]["avatar_id"]
    _code(inst, 'SetKnockedOut(_dead);', human)
    def pressure():
        return float(_probe(inst, '""+Online_ChallengePressure()', "level"))
    wait(lambda: math.isclose(pressure(), -0.025, abs_tol=1e-5), "sole human loss enables one local retry ease")
    _code(inst, 'Recover();', human)
    time.sleep(0.5)
    _code(inst, 'Reset();', "level", "level_specific")  # dismiss the map's normal defeat GUI on retry
    _code(inst, 'Online_ChallengeEncounterWin("solo_retry");', "level")
    wait(lambda: math.isclose(pressure(), 0.08, abs_tol=1e-5), "solo win clears local relief and increases general pressure")
    _code(inst, 'Online_ChallengeEncounterWin("solo_retry");', "level")
    assert math.isclose(pressure(), 0.08, abs_tol=1e-5)
    base = bridge.me()["pos"]
    coverage_actor = bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                                 pos=[base[0] + 3, base[1] + 0.2, base[2] + 3], teams="armor_fixture")["object_id"]
    _code(inst, 'this_mo.static_char=true;combat_allowed=false;hostile=false;invincible=true;', coverage_actor)
    time.sleep(0.4)
    assert _probe(inst, f'""+Online_AuthoredArmorRegions({coverage_actor})', "level") == "0", "armor fixture is not bare"
    (Path(ctx.out_dir) / "pre_hud_status.json").write_text(__import__("json").dumps(bridge.status(), indent=2))
    wait(lambda: bridge.status()["difficulty_hud_layout"].get("drawn", False), "single-view scaling readout")
    ctx.shot(inst, "private_solo_performance")
    metrics = {"snapshot": bridge.status()["online"]["challenge"], "hud": bridge.status()["difficulty_hud"]}
    coverage = []
    for localized in (True, False, True):
        _code(inst, 'SetConfigValueBool("armor_localized",' + str(localized).lower() + ');', "level")
        bridge.call("level_messages", prefix="mode_setting_probe")
        _code(inst, 'int old_regions=campaign_armor_regions;bool old_metal=g_wearing_metal_armor;'
              'campaign_armor_regions=4;g_wearing_metal_armor=false;'
              'vec3 head=this_mo.rigged_object().GetAvgIKChainPos("head");'
              'vec3 torso=this_mo.rigged_object().GetAvgIKChainPos("torso");'
              'level.SendMessage("mode_setting_probe "+CampaignArmorProtectsAt(head)+"|"+CampaignArmorProtectsAt(torso));'
              'campaign_armor_regions=old_regions;g_wearing_metal_armor=old_metal;', coverage_actor)
        result = bridge.call("level_messages", prefix="mode_setting_probe")["messages"][-1].split(" ", 1)[1]
        assert result == ("true|false" if localized else "true|true"), (localized, result)
        coverage.append({"localized": localized, "head_torso": result})
    metrics["actual_armor_coverage"] = coverage
    def personal(code):
        return bridge.call("exec", target="player_menu", local_player=0, code=code)
    def personal_probe(expression):
        personal('SetConfigValueString("test_mode_setting_probe",""+(' + expression + '));SaveConfig();')
        values = dict(line.split(": ", 1) for line in
                      (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        return values["test_mode_setting_probe"]
    bridge.call("input", controller=0, hold={"quit": 1}, steps=1)
    wait(lambda: 0 in bridge.status()["open_menu_players"], "solo host opens Start menu")
    personal('imGUI.receiveMessage(IMMessage("Settings"));')
    wait(lambda: personal_probe("personal_settings") == "true", "actual personal settings context")
    personal('SwitchSettingsScreen("Game");')
    wait(lambda: personal_probe("current_screen") == "Game", "Game settings page")
    personal('imGUI.receiveMessage(IMMessage("open_difficulty_settings"));')
    wait(lambda: personal_probe("current_screen") == "Difficulty", "Game Difficulty submenu compiles and opens")
    personal('string names;for(uint i=0;i<gui_elements.length();++i)names+=gui_elements[i].name+"|";'
             'SetConfigValueString("test_mode_setting_probe",names);SaveConfig();')
    labels = dict(line.split(": ", 1) for line in
                  (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)["test_mode_setting_probe"].split("|")
    for label in ("Difficulty mode", "Starting preset", "Armor protection", "Party stats and reinforcements", "Physical armor",
                  "Weapons for extra enemies", "Performance scaling"):
        assert label in labels, (label, labels)
    personal('string names;for(uint i=0;i<controller_items.length();++i)names+=controller_items[i].getName()+"|";'
             'SetConfigValueString("test_mode_setting_cards",names);')
    cards=personal_probe('GetConfigValueString("test_mode_setting_cards")').split("|")
    for key in ("campaign_ai_scaling_rate","challenge_time","challenge_performance_rate"):
        for rate in range(5):
            assert f"rate_{key}_{rate}" in cards, (key, rate, cards)
    for rate in (4,0):
        personal('IMMessage m("difficulty_rate");m.addString("campaign_ai_scaling_rate");'
                 f'm.addInt({rate});imGUI.receiveMessage(m);')
        wait(lambda: personal_probe('GetConfigValueInt("campaign_ai_scaling_rate")') == str(rate),
             "actual AI card callback edits the live host rate")
        assert personal_probe('GetConfigValueBool("campaign_ai_scaling")') == str(rate != 0).lower()
    personal('IMMessage m("option_changed");m.addString("Performance scaling");m.addString("On win");imGUI.receiveMessage(m);')
    wait(lambda: personal_probe('""+LobbyGetModeSetting("challenge_performance")') == "1", "actual performance dropdown edits live host rule")
    personal('IMMessage m("option_changed");m.addString("Performance scaling");m.addString("Forced-50");imGUI.receiveMessage(m);')
    wait(lambda: personal_probe('""+LobbyGetModeSetting("challenge_performance")') == "3", "performance dropdown restores Forced50")
    assert math.isclose(pressure(), 0.08, abs_tol=1e-5), "live rule edit repriced an already credited victory"
    ctx.shot(inst, "actual_game_difficulty_settings")
    metrics["actual_game_difficulty_settings"] = labels
    personal('this_ui.SendCallback("close");')
    wait(lambda: not bridge.status()["open_menu_players"], "personal settings closes")
    _code(inst, 'SetConfigValueBool("difficulty_scaling",false);', "level")
    wait(lambda: pressure() == 0 and not bridge.status()["difficulty_hud_layout"].get("drawn", False),
         "Traditional immediately suspends the private solo modifiers")
    bridge.call("disconnect")
    closed_states = []
    def private_closed():
        state = bridge.status()
        closed_states.append({"loaded": state["level_loaded"], "active": state["online"]["active"],
                              "hosting": state["online"]["hosting"]})
        assert not state["online"]["hosting"], "private Leave automatically adopted the old world again"
        return not state["online"]["active"] and not state["level_loaded"]
    wait(private_closed, "private run closes to the main menu without immediate re-adoption")
    metrics["private_close_transition"] = closed_states
    _code(inst, 'SetConfigValueBool("difficulty_scaling",true);')
    bridge.call("load_level", path=SANDBOX_MAP)
    _ready(inst)
    wait(lambda: bridge.status()["online"]["hosting"], "fresh direct solo run is adopted")
    fresh = bridge.status()["online"]["challenge"]
    assert fresh["pressure"] == 0 and fresh["wipes"] == [] and fresh["wipe_serial"] == 0, fresh
    assert not any("solo_retry" in key for key in fresh["encounters"]), fresh
    metrics["fresh_private_run"] = fresh
    assert not script_errors(inst), script_errors(inst)[:8]
    assert not bridge.call("nan_scan").get("characters")
    return {"passed": True, "metrics": metrics, "notes": []}
