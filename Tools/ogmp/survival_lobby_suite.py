"""Start Survival from Play, choose a sandbox map and share it with a real guest."""
import json
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _ready
from .scenarios import script_errors


def survival_lobby(ctx):
    host = ctx.instance("survival_lobby_host", keys={**RENDER_KEYS, "mp_port": "29341"})
    guest = ctx.instance("survival_lobby_guest", keys={**RENDER_KEYS, "mp_port": "29342"})
    mode = "og-online-survival"

    def play_card():
        host.bridge.call("menu", script="play_menu.as")
        _code(host, 'int ix=-1; for(uint i=0;i<play_menu.size();++i) '
                    'if(play_menu[i].file=="survival") ix=i; SetConfigValueInt("test_survival_card",ix);')
        return int(_probe(host, 'GetConfigValueInt("test_survival_card")'))

    def toggle_mod(enabled):
        host.bridge.call("menu", script="mainmenu.as")
        _code(host, 'array<ModID>@ ids=GetModSids(); for(uint i=0;i<ids.length();++i) '
                    f'if(ModGetID(ids[i])=="{mode}") ModActivation(ids[i],{str(enabled).lower()});')

    assert play_card() >= 0
    toggle_mod(False)
    assert play_card() == -1
    toggle_mod(True)
    card_index = play_card()
    assert card_index >= 0
    _code(host, f'IMMessage click("run_file","survival"); click.addInt({card_index}); imGUI.receiveMessage(click);')
    host.bridge.wait(lambda: host.bridge.status()["online"]["lobby_mode"] == "survival", 20,
                     what="Play opens a local Survival lobby")
    host.bridge.call("menu", script="multiplayer_lobby.as")
    assert _probe(host, "LobbyIsLocalOnly()") == "true"
    selected = 'lobby_modes[LobbySelectedMode()]'
    assert _probe(host, selected + '.id') == "survival"
    _code(host, 'int missing=0;int stock=0;for(uint i=0;i<lobby_modes.length();++i){'
                'if(lobby_modes[i].id!="sandbox")continue;stock=lobby_modes[i].maps.length();'
                'for(int j=0;j<stock;++j){bool found=false;'
                f'for(uint k=0;k<{selected}.maps.length();++k) '
                f'if(lobby_modes[i].maps[j].path=={selected}.maps[k].path) found=true;'
                'if(!found)missing++;}}SetConfigValueInt("test_missing_survival_maps",missing);'
                'SetConfigValueInt("test_sandbox_maps",stock);')
    assert int(_probe(host, 'GetConfigValueInt("test_sandbox_maps")')) > 1
    assert _probe(host, 'GetConfigValueInt("test_missing_survival_maps")') == "0"
    _code(host, f'for(uint j=0;j<{selected}.maps.length();++j) if({selected}.maps[j].path=="{SANDBOX_MAP}")'
                ' LobbyChooseMap(LobbySelectedMode(),j);')
    assert _probe(host, "LobbyGetMap()") == SANDBOX_MAP
    assert _probe(host, "LobbyOpenToNetwork()") == "true"
    # The host can play this network lobby alone. Finish a battery, lose the
    # next one, then use Continue and start again with the same lobby settings.
    owner = host.bridge.status()["online"]["players"][0]["id"]
    saved_rules = 'LobbyGetModeSetting("survival_seed")==48003 && LobbyGetModeSetting("survival_starting_race")==2'
    assert _probe(host, 'LobbySetModeSetting("progression",1) && LobbySetModeSetting("progression_gain_round",1) '
                        '&& LobbySetModeSetting("progression_shop",0) && LobbySetModeSetting("survival_seed",48003) '
                        '&& LobbySetModeSetting("survival_starting_race",2)') == "true"
    assert _probe(host, "LobbyStart()") == "true"
    _ready(host, SANDBOX_MAP)

    def state():
        return json.loads(_probe(host, f'SessionGetModData("{mode}")', "level"))

    def live_opponents():
        return [r for r in host.bridge.state() if "survival_enemies" in r["teams"] and r["knocked_out"] == 0]

    def wait(check, what):
        return host.bridge.wait(check, 45, step=.1, what=what)

    target = wait(live_opponents, "solo opening opponent")[0]
    _code(host, "SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", target["id"])
    wait(lambda: state().get("phase") == 2, "solo battery intermission")
    assert host.bridge.call("progression", actor=owner)["view"]["held"] >= 25
    _code(host, "due=the_time;", "level", "level_specific")
    wait(lambda: state().get("battery") == 2 and live_opponents(), "solo second battery")
    _code(host, "SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", host.bridge.me()["id"])
    wait(lambda: state().get("phase") == 3, "sole player gets a defeat camera hold")
    assert live_opponents()
    wait(lambda: state().get("phase") == 5, "solo results")
    wait(lambda: "Team 1" in _probe(host, "score_text", "level", "level_specific"), "solo score on screen")
    # Replay from the scoreboard clears committed progression and recreates the
    # original lobby roster on battery one. The pause-menu callback uses the same path.
    host.bridge.call("pointer", x=200, y=100, buttons=1)
    host.bridge.call("pointer", x=200, y=100, buttons=0)
    wait(lambda: host.bridge.status()["level_loaded"] and live_opponents() and state().get("battery") == 1
         and state().get("launched") == 1, "left click starts a fresh run")
    assert host.bridge.call("progression", actor=owner)["view"]["held"] == 0
    _code(host, "SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", host.bridge.me()["id"])
    wait(lambda: state().get("phase") == 5, "second scoreboard")
    _code(host, "OnlineRetry();", "level")
    wait(lambda: host.bridge.status()["level_loaded"] and live_opponents() and state().get("battery") == 1
         and state().get("launched") == 1, "menu Retry from results starts a fresh run")
    _code(host, "SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", host.bridge.me()["id"])
    wait(lambda: "Right click" in _probe(host, "score_text", "level", "level_specific"), "host results controls")
    host.bridge.call("pointer", x=200, y=100, buttons=4)
    host.bridge.call("pointer", x=200, y=100, buttons=0)
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"] and not host.bridge.status()["level_loaded"],
         "sole human returns to a network lobby")
    save = Path(host.writedir, "Data/SessionSaves/slot0.json")
    assert json.loads(save.read_text()).get("finished") is True
    assert _probe(host, 'LobbySetModeSetting("progression_gain_round",0) && LobbySetModeSetting("survival_seed",7) '
                        '&& LobbySetModeSetting("survival_starting_race",1)') == "true"
    assert _probe(host, "SessionLoad(0)") == "true"
    wait(lambda: _probe(host, f'LobbyGetModeSetting("progression_gain_round")==1 && {saved_rules}') == "true",
         "Continue restores the finished run's configured lobby")
    assert _probe(host, "LobbyGetMap()") == SANDBOX_MAP
    guest.bridge.call("connect", ip="127.0.0.1:29341")
    host.bridge.wait(lambda: len(guest.bridge.status()["online"]["players"]) == 2, 40, what="guest joins")
    guest_id = next(row["id"] for row in host.bridge.status()["online"]["players"] if not row["host"])
    assert _probe(host, f'LobbySetPlayerTeam({guest_id},2)') == "true"
    assert _probe(host, "LobbyStart()") == "true"
    for peer, team in [(host, 1), (guest, 2)]:
        _ready(peer, SANDBOX_MAP)
        assert _probe(peer, 'GetCurrCampaignID()==""', "level") == "true"
        assert _probe(peer, saved_rules, "level") == "true"
        peer.bridge.wait(lambda: (peer.bridge.me() or {}).get("teams") == f"lobby_team_{team}", 20,
                         what="roster team on the controlled character")
    wait(lambda: state().get("battery") == 1 and state().get("launched") == 1, "new game starts at battery one")
    progression = host.bridge.call("progression", actor=owner)
    assert progression["ledger"]["completed_rounds"] == 0 and progression["view"]["held"] == 0
    assert _probe(host, "SessionSave(1)", "level") == "true"
    card = Path(host.writedir, "Data/SessionSaves/slot1.json")
    host.bridge.wait(card.exists, 20, what="Survival save")
    saved = json.loads(card.read_text())
    assert saved["mode"] == "survival" and saved["mode_name"] == "Survival" and saved["lobby"]
    assert saved["campaign"] == "" and mode in saved["session_mods"]
    rules = saved["rules"]["mode_settings"]["survival"]
    assert (rules["survival_seed"], rules["survival_starting_race"]) == (48003, 2)
    assert _probe(host, "SessionLoad(1)", "level") == "true"
    for peer in (host, guest):
        wait(lambda: peer.bridge.status()["online"]["lobby_waiting"] and not peer.bridge.status()["level_loaded"],
             "saved Survival lobby")
        peer.bridge.call("menu", script="multiplayer_lobby.as")
        wait(lambda: _probe(peer, saved_rules) == "true", "peer receives saved race and seed")
    assert _probe(host, selected + '.id') == "survival" and _probe(host, "LobbyGetMap()") == SANDBOX_MAP
    assert not script_errors(host) and not script_errors(guest), (script_errors(host), script_errors(guest))
    return {"passed": True, "metrics": {"teams": 2, "restored_map": SANDBOX_MAP}, "notes": []}
