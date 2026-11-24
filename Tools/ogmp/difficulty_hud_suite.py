"""Host-view difficulty readout against actual campaign NPC modifiers."""

import math
import time
import traceback
from pathlib import Path

from .campaign_scaling_suite import CAMPAIGN, FACTORS, RENDER_KEYS
from .newcampaign_local_suite import FIRST, _ready, _skip
from .scenarios import script_errors


def difficulty_hud(ctx):
    host = ctx.instance("difficulty_hud_host", width=1280, height=720,
                        keys={**RENDER_KEYS, "mp_port": "28084", "tutorials": "false", "auto_ledge_grab": "false",
                              "player_invincible": "true", "difficulty_preset": "Expert",
                              "difficulty_preset_value": "3", "game_difficulty": "1",
                              "difficulty_scaling": "true", "campaign_mp_scaling": "false", "campaign_ai_scaling": "false",
                              "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false", "show_difficulty_readout": "true"})
    guest = ctx.instance("difficulty_hud_guest", width=640, height=360,
                         keys={**RENDER_KEYS, "mp_port": "28085", "player_invincible": "true"})
    metrics = {}

    def code(inst, text, target="level", **kwargs):
        return inst.bridge.call("exec", target=target, code=text, **kwargs)

    def status(inst=host):
        return inst.bridge.status()

    def rows(inst=host):
        return status(inst)["online"]["players"]

    def wait(predicate, label, seconds=30):
        return host.bridge.wait(predicate, seconds, step=0.08, what=label)

    def data(inst=host):
        return status(inst)["difficulty_hud"]

    def layout(inst=host):
        return status(inst)["difficulty_hud_layout"]

    def probe(text, target="level", **kwargs):
        code(host, 'SetConfigValueString("test_difficulty_hud_probe",""+(' + text + '));SaveConfig();', target, **kwargs)
        values = dict(line.split(": ", 1) for line in
                      (Path(host.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        return values["test_difficulty_hud_probe"]

    def owner(index):
        code(host, f'SetConfigValueInt("controller_menu_owner",{index});')

    def actors(inst=host):
        return {c["common_id"]: c for c in inst.bridge.state()}

    def applied_matches():
        current = data().get("current", {})
        world = actors()
        remote_world = actors(guest)
        listed = current.get("actors", [])
        if not listed:
            return False
        return all(a["id"] in world and a["id"] in remote_world and
                   all(math.isclose(a[key], world[a["id"]]["combat_stats"][field], abs_tol=1e-4) and
                       math.isclose(a[key], remote_world[a["id"]]["combat_stats"][field], abs_tol=1e-4)
                       for key, field in zip(("vitality", "damage", "armor", "ai"), FACTORS)) for a in listed)

    def assert_layout(screen):
        wait(lambda: layout().get("drawn", False) and layout().get("screen") == screen,
             "HUD draws only in the designated host's assigned screen")
        state, shown, snapshot = status(), layout(), data()
        host_row = next(p for p in state["online"]["players"] if p["host"])
        host_actor = next(c for c in actors().values() if c["id"] == host_row["avatar_id"])
        assert shown["host_player"] == host_row["id"] == p2["id"]
        assert shown["local_index"] == 1 and shown["camera"] == host_actor["camera_id"]
        viewport = [0, 360 * (screen - 1), 1280, 360 * screen]
        assert all(abs(a - b) <= 1 for a, b in zip(shown["viewport"], viewport)), shown
        x1, y1, x2, y2 = shown["rect"]
        left, top, right, bottom = shown["viewport"]
        assert left <= x1 < x2 <= right and top <= y1 < y2 <= bottom, shown
        assert 5 <= right - x2 <= 24 and 5 <= y1 - top <= 24, shown
        assert x2 - x1 <= 550 and y2 - y1 <= 300, shown
        wait(lambda: layout().get("rows") == data().get("lines"), "rendered rows use the current production snapshot")
        assert 3 <= len(snapshot["lines"]) <= 6, snapshot["lines"]
        assert not data(guest).get("visible", False) and not layout(guest).get("drawn", False)
        assert not any(w["name"] == "##host_difficulty_conditions" for w in guest.bridge.call("ui_state")["windows"])
        return shown

    def close_menu(local):
        code(host, 'this_ui.SendCallback("close");', "player_menu", local_player=local)
        wait(lambda: local not in status()["open_menu_players"], "personal menu closes")

    def drain_dialogue(label):
        deadline, quiet_since = time.monotonic() + 30, None
        while time.monotonic() < deadline:
            pending = int(probe('dialogue_queue.length()'))
            state = status()
            if pending or state["dialogue_camera_control"] or state["coop_cutscene"]:
                quiet_since = None
                _skip(host)
            else:
                quiet_since = quiet_since or time.monotonic()
                if time.monotonic() - quiet_since > 0.6:
                    return
            time.sleep(0.1)
        raise AssertionError("campaign dialogue queue did not drain: " + label)

    def checkpoint(number, expected):
        trigger = 477 if number == 0 else 85
        code(host, f'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint {number} 0 {trigger}");')
        def advanced():
            code(host, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
                 'if(h.GetID()==14)SetConfigValueString("test_difficulty_hud_probe",""+h.GetIntVar("progress"));}SaveConfig();')
            values = dict(line.split(": ", 1) for line in
                          (Path(host.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
            return values["test_difficulty_hud_probe"] == str(expected)
        wait(advanced, "authored campaign checkpoint advances")
        drain_dialogue("checkpoint " + str(number))

    def toggle_display_menu(expected):
        host.bridge.call("input", controller=1, hold={"quit": 1}, steps=1)
        wait(lambda: 1 in status()["open_menu_players"], "host opens their actual Game settings")
        code(host, 'imGUI.receiveMessage(IMMessage("Settings"));', "player_menu", local_player=1)
        wait(lambda: probe('personal_settings', "player_menu", local_player=1) == "true", "host settings page")
        code(host, 'SwitchSettingsScreen("Game");', "player_menu", local_player=1)
        assert probe('current_screen', "player_menu", local_player=1) == "Game"
        assert probe('""+GetConfigValueBool("show_difficulty_readout")', "player_menu", local_player=1) == str(not expected).lower()
        code(host, 'for(uint i=0;i<gui_elements.length();++i){if(gui_elements[i].config_name=="show_difficulty_readout"){'
             'IMMessage m("option_changed");m.addString(gui_elements[i].name);m.addString("");imGUI.receiveMessage(m);break;}}',
             "player_menu", local_player=1)
        wait(lambda: probe('GetConfigValueBool("show_difficulty_readout")', "player_menu", local_player=1) == str(expected).lower(),
             "actual readout checkbox changes the display preference")
        close_menu(1)

    try:
        code(host, 'LobbyBeginHost();LobbyAddLocalPlayer();LobbyAddBot();'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
             'LobbySetModeSetting("challenge_time",0);LobbySetModeSetting("challenge_round",0);'
             'LobbySetModeSetting("challenge_performance",1);LobbySetModeSetting("challenge_performance_rate",2);', "menu")
        guest.bridge.call("connect", ip="127.0.0.1:28084")
        wait(lambda: len(rows()) == len(rows(guest)) == 4, "three humans and one friendly bot")
        p2 = next(p for p in rows() if p["peer_id"] == 255 and p["local_index"] == 1)
        assert probe(f'LobbyPromoteHost({p2["id"]})', "menu") == "true"
        code(host, 'SetConfigValueInt("controller_menu_owner",1);LobbyStart();', "menu")
        _ready(host, FIRST)
        _ready(guest, FIRST)
        wait(lambda: status()["coop_cutscene"], "real opening campaign cutscene")
        assert not layout().get("drawn", False), "HUD covers a campaign cutscene"
        drain_dialogue("opening scene")
        wait(lambda: not status()["coop_cutscene"] and status()["render_views"] == 2, "two couch views resume")
        checkpoint(0, 1)
        checkpoint(1, 2)
        code(host, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(!c.controlled)c.ReceiveScriptMessage("set_combat_allowed false");'
             'if(Online_IsPlayerCharacter(c.GetID()))c.static_char=true;}'
             'MovementObject@ hero=ReadCharacterID(0);MovementObject@ enemy=ReadCharacterID(363);'
             'hero.position=Online_SafeSpotNear(enemy.position,enemy.GetFacing(),12);hero.Execute("FixDiscontinuity();SetOnGround(true);");')
        wait(lambda: data().get("current", {}).get("known") and data()["current"]["goal"] == 2,
             "HUD resolves the current authored fight")
        wait(lambda: data().get("next", {}).get("known") and data()["next"]["goal"] == 4,
             "HUD previews the next authored fight")
        wait(lambda: layout().get("drawn", False), "readout appears after the dialogue queue is empty")
        ctx.shot(host, "host_p2_screen2")
        wait(applied_matches, "HUD current factors match actual installed NPC modifiers")
        assert data()["party"]["humans"] == 3 and data()["party"]["bots"] == 1
        assert data()["base"]["name"] == "Expert" and not data()["party"]["stats_enabled"]
        metrics["unscaled_fight"] = data()
        metrics["host_screen2"] = assert_layout(2)
        ctx.shot(guest, "guest_no_readout")

        owner(1)
        code(host, 'SetConfigValueBool("campaign_mp_scaling",true);Online_CaptureEnemyDifficulty();')
        wait(lambda: data()["party"]["stats_enabled"] and data()["current"]["extras_spawned"] == 2,
             "party scaling adds the real first encounter's two reinforcements")
        wait(applied_matches, "HUD reads applied party factors rather than a stale forecast")
        scaled = data()
        assert scaled["current"]["threat"] > metrics["unscaled_fight"]["current"]["threat"]
        assert scaled["next"]["extras_expected"] == 2 and scaled["current"]["extras_alive"] == 2
        metrics["party_scaled"] = scaled

        # Display preference changes presentation, preserving all modifiers.
        toggle_display_menu(False)
        wait(lambda: not layout().get("drawn", False), "display checkbox hides the readout")
        time.sleep(0.3)
        assert not layout().get("drawn", False), "readout reappeared after closing its disabled setting"
        assert data()["party"]["stats_enabled"] and data()["progression"]["pressure"] == 0
        wait(applied_matches, "hiding the display leaves applied difficulty unchanged")
        toggle_display_menu(True)
        assert_layout(2)

        owner(1)
        code(host, 'Online_ChallengeEncounterBegin("difficulty_hud_probe");Online_ChallengeEncounterWin("difficulty_hud_probe");')
        wait(lambda: math.isclose(data()["progression"]["pressure"], 0.04, abs_tol=0.0002), "encounter victory updates progression")
        time.sleep(1.0)
        assert math.isclose(data()["current"]["threat"],scaled["current"]["threat"],abs_tol=.0001)
        assert data()["progression"]["applied_pressure"] == 0, "HUD repriced the current fight after a win"
        code(host, 'Online_CaptureEnemyDifficulty();')
        wait(lambda: data()["current"]["threat"] > scaled["current"]["threat"], "applied encounter estimate reflects progression")
        wait(applied_matches, "HUD follows the authoritative progression snapshot")
        assert data()["current"]["threat"] > scaled["current"]["threat"]
        assert data()["progression"]["next_pressure"] >= data()["progression"]["pressure"]
        metrics["progression_scaled"] = data()

        code(host, 'SetConfigValueBool("campaign_mp_scaling",false);Online_CaptureEnemyDifficulty();')
        wait(lambda: not data()["party"]["stats_enabled"], "party scaling disabled live")
        wait(lambda: data()["current"]["threat"] < metrics["progression_scaled"]["current"]["threat"],
             "applied encounter estimate reflects disabled party scaling")
        wait(applied_matches, "disabled party scaling still shows actual retained enemies and progression")
        assert data()["current"]["extras_spawned"] == 2 and data()["next"]["extras_expected"] == 0
        assert data()["current"]["threat"] < metrics["progression_scaled"]["current"]["threat"]
        metrics["party_disabled_with_existing_enemies"] = data()

        owner(0)  # P1 stays this computer's local leader despite P2 hosting.
        assert probe(f'LobbySetPlayerScreen({p2["id"]},1)') == "true"
        metrics["host_screen1"] = assert_layout(1)
        ctx.shot(host, "host_p2_moved_screen1")
        host.bridge.call("input", controller=0, hold={"quit": 1}, steps=1)
        wait(lambda: 0 in status()["open_menu_players"], "non-host P1 opens a personal menu")
        assert_layout(1)
        close_menu(0)
        host.bridge.call("input", controller=1, hold={"quit": 1}, steps=1)
        wait(lambda: 1 in status()["open_menu_players"], "designated host P2 opens their personal menu")
        wait(lambda: not layout().get("drawn", False), "host menu hides its difficulty overlay")
        close_menu(1)
        assert_layout(1)

        guest.bridge.call("disconnect")
        wait(lambda: len(rows()) == 3, "guest leaves the session")
        owner(1)
        code(host, 'LobbyEndGame();')
        wait(lambda: status()["online"]["lobby_waiting"] and not status()["level_loaded"], "return to lobby")
        code(host, 'SetConfigValueInt("controller_menu_owner",1);', "menu")
        assert probe('LobbyPromoteHost(255)', "menu") == "true"
        code(host, 'SetConfigValueInt("controller_menu_owner",0);', "menu")
        code(host, ''.join(f'LobbyRemovePlayer({p["id"]});' for p in rows() if p["id"] != 255) +
             'SetConfigValueBool("campaign_mp_scaling",false);LobbySetModeSetting("challenge_performance",0);LobbyStart();', "menu")
        _ready(host, FIRST)
        drain_dialogue("solo opening scene")
        wait(lambda: not status()["coop_cutscene"], "solo intro ends")
        wait(lambda: not data().get("scaled_context", True) and not layout().get("drawn", False),
             "unscaled solo gameplay has no readout despite its enabled display setting")
        code(host, 'SetConfigValueBool("campaign_mp_scaling",true);Online_CaptureEnemyDifficulty();')
        time.sleep(0.5)
        assert not data()["scaled_context"] and not layout().get("drawn", False), "solo party toggle showed an unchanged difficulty"
        metrics["unscaled_solo"] = data()
        for inst in (host, guest):
            assert not script_errors(inst), (inst.name, script_errors(inst))
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        failure = traceback.format_exc()
        try:
            metrics["failure_host_status"] = status()
            metrics["failure_guest_status"] = status(guest)
        except Exception:
            pass
        return {"passed": False, "metrics": metrics, "notes": [failure]}
    finally:
        for inst in (host, guest):
            inst.stop()
            (Path(ctx.out_dir) / (inst.name + ".log")).write_text(inst.log())
