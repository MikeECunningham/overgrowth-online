"""Two couch seats browse separately, bank XP, buy upgrades and equip in the shared shop."""
import json
import time
from pathlib import Path

from .campaign_scaling_suite import CAMPAIGN, RENDER_KEYS
from .newcampaign_local_suite import FIRST, _join_pad, _probe, _ready, _skip
from .scenarios import script_errors


def progression_free_buff_ui(ctx):
    """Checkpoint hands accept keyboard and D-pad input independently of the paid shop."""
    host = ctx.instance("progression_free_buff_ui", width=1024, height=720,
                        keys={**RENDER_KEYS, "mp_port": "28614", "player_invincible": "true", "tutorials": "false"})
    bridge = host.bridge

    def script(code, target="level"):
        return bridge.call("exec", target=target, code=code)

    def panels():
        return sorted(bridge.status()["progression_ui"], key=lambda panel: panel["screen"])

    def data(actor):
        return bridge.call("progression", actor=actor)

    def wait(condition, label):
        return bridge.wait(condition, 30, step=.1, what=label)

    def tap_pad(button):
        for down in (True,False):
            bridge.call("virtual_pad", action="button", slot=0, button=button, down=down)
            time.sleep(.12)

    def check_choice(actor, key, before):
        result = data(actor)
        assert before["view"]["held"] == result["view"]["held"], (key, before, result)
        def account(state):
            return next(p for p in state["ledger"]["players"] if p["id"] == actor)
        if key.startswith("armor_"):
            slot = int(key.split("_")[1])
            assert account(result)["armor"][slot] > account(before)["armor"][slot]
        elif "owned_" + key in result["view"] or account(result)["weapons"] != account(before)["weapons"]:
            assert account(result)["weapons"] != account(before)["weapons"]
        elif key == "race_upgrade":
            assert account(result)["race"] != account(before).get("race",-1)
        elif key == "recruit":
            assert len(result["ledger"]["players"]) > len(before["ledger"]["players"])
        elif key == "ai":
            def ranks(state):
                return sum(p["ai"] for p in state["ledger"]["players"])
            assert ranks(result) == ranks(before) + 1
        elif key == "restore_health":
            avatar = next(p["avatar_id"] for p in bridge.status()["online"]["players"] if p["id"] == actor)
            fighter = next(c for c in bridge.state() if c["id"] == avatar)
            assert all(fighter[vital] >= .999 for vital in ("permanent_health", "temp_health", "blood_health"))
        else:
            assert account(result)[key] == account(before)[key] + 1

    script("LobbyBeginHost();", "menu")
    _join_pad(host, 0, 2)
    script(f'SetControllerPlayer(0);LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
           'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shop",1);'
           'LobbySetModeSetting("progression_free_buffs",1);LobbySetModeSetting("progression_shared",0);'
           'LobbySetModeSetting("progression_shop_heal",1);LobbySetModeSetting("progression_timer",0);LobbyStart();', "menu")
    _ready(host, FIRST)
    _skip(host)
    wait(lambda: len(panels()) == 2, "two local players")
    # A real host save with max-rank Luck removes randomness from these input checks.
    path = Path(host.writedir, "Data/SessionSaves/slot2.json")
    script("SessionSave(2);")
    wait(path.is_file, "save before checkpoint")
    saved = json.loads(path.read_text())
    roster = saved["session_mods"]["og-online-progression"]["runtime"]["players"]
    assert len(roster) == 2
    for player in roster:
        player["luck"] = 50
    path.write_text(json.dumps(saved))
    script("SessionLoad(2);")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "roster restored")
    script("SetControllerPlayer(0);LobbyStart();", "menu")
    _ready(host, FIRST)
    _skip(host)
    script("ProgressionCheckpoint(31);")
    wait(lambda: len(panels()) == 2 and all(p.get("page") == "reward" for p in panels()), "both checkpoint hands")
    rows = panels()
    assert all(not p["shop"] and len(p["visible_cards"]) == len(set(p["visible_cards"])) == 4 for p in rows), rows
    p1, p2 = [p["actor"] for p in rows]
    first, second = data(p1), data(p2)
    assert [c["id"] for c in first["view"]["reward_cards"]] == rows[0]["visible_cards"]
    assert not bridge.status()["open_menu_players"]
    ctx.shot(host, "checkpoint_free_choices")
    bridge.call("key", key="4")
    wait(lambda: not data(p1)["view"]["reward_pending"], "number row claims the fourth upgrade")
    check_choice(p1, rows[0]["visible_cards"][3], first)
    assert data(p2)["view"]["reward_pending"]
    assert not panels()[0].get("page") and panels()[1].get("page") == "reward"
    tap_pad(4)  # open P2's personal menu
    wait(lambda: bridge.status()["open_menu_players"] == [1], "controller opens personal menu during pending hand")
    assert panels()[1].get("page") != "reward"
    tap_pad(14)  # D-pad right can navigate the menu without selecting a reward
    assert data(p2)["view"]["reward_pending"]
    bridge.call("exec", target="player_menu", local_player=1, code='this_ui.SendCallback("close");')
    wait(lambda: panels()[1].get("page") == "reward", "personal menu returns to the pending hand")
    tap_pad(14)
    wait(lambda: not data(p2)["view"]["reward_pending"], "P2's D-pad claims the second upgrade")
    check_choice(p2, rows[1]["visible_cards"][1], second)
    script("ProgressionCheckpoint(31);")
    assert not data(p1)["view"]["reward_pending"] and not data(p2)["view"]["reward_pending"]
    script("ProgressionCheckpoint(32);")
    wait(lambda: all(p.get("page") == "reward" for p in panels()), "next checkpoint offers new hands")
    before_keypad = data(p1)
    second_card = panels()[0]["visible_cards"][1]
    bridge.call("key", key="Keypad 2")
    wait(lambda: not data(p1)["view"]["reward_pending"], "keypad selects from P1's hand")
    check_choice(p1, second_card, before_keypad)
    remaining = data(p2)["view"]["reward_generation"]
    script("ProgressionBeginShop(false);")
    wait(lambda: all(p.get("page") == "shop" for p in panels()), "paid shop opens independently")
    for actor in (p1,p2):
        assert bridge.call("progression", actor=actor, action="ready")["submitted"]
    wait(lambda: panels()[1].get("page") == "reward", "unclaimed hand resumes when paid shop closes")
    assert data(p2)["view"]["reward_generation"] == remaining
    assert not data(p1)["view"]["reward_pending"], "end shop unexpectedly awarded another free hand"
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"keyboard_slots": [4,2], "controller_slot": 2}, "notes": []}


def progression_ui(ctx):
    host = ctx.instance("progression_shop_ui", width=1280, height=720,
                        keys={**RENDER_KEYS, "mp_port": "28119", "player_invincible": "true",
                              "tutorials": "false", "auto_ledge_grab": "false"})
    bridge = host.bridge

    def script(code, target="level", **args):
        return bridge.call("exec", target=target, code=code, **args)

    def rows():
        return sorted(bridge.status().get("progression_ui", []), key=lambda r: r["screen"])

    def wait(fn, what):
        return bridge.wait(fn, 30, step=0.06, what=what)

    def pad(button):
        bridge.call("virtual_pad", action="button", slot=0, button=button, down=True)
        time.sleep(.12)
        bridge.call("virtual_pad", action="button", slot=0, button=button, down=False)
        time.sleep(.1)

    def stick(value):
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=value)

    def settings_offset():
        script('SetConfigValueInt("test_progression_scroll",show_keybinds_offset);SaveConfig();',
               "player_menu", local_player=1)
        for line in (Path(host.writedir)/"Data/config.txt").read_text().splitlines():
            if line.startswith("test_progression_scroll: "):
                return int(line.split(": ",1)[1])
        return 0

    def host_form():
        return next((w for w in bridge.call("ui_state")["windows"] if w["name"] == "Hosting options"), {})

    def press(seat, key):
        bridge.call("input", controller=seat, hold={key: 1}, steps=8)
        time.sleep(.18)

    try:
        script("LobbyBeginHost();", "menu")
        _join_pad(host, 0, 2)
        script('SetControllerPlayer(0);LobbyAddBot();'
               f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
               'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);'
               'LobbySetModeSetting("progression_shop_heal",1);LobbySetModeSetting("progression_timer",0);LobbyStart();', "menu")
        _ready(host, FIRST)
        _skip(host)
        wait(lambda: len(rows()) == 2, "both seat XP readouts")
        pad(4)
        wait(lambda: bridge.status()["open_menu_players"] == [1], "P2 pause menu")
        script('OpenPersonalSettings();', "player_menu", local_player=1)
        stick(30000)
        wait(lambda: settings_offset() >= 2, "P2 right stick scrolls gamepad bindings")
        stick(-30000)
        wait(lambda: settings_offset() == 0, "P2 scrolls bindings back up")
        stick(0)
        script('this_ui.SendCallback("close");', "player_menu", local_player=1)
        wait(lambda: not bridge.status()["open_menu_players"], "settings closed")
        # Drive the public encounter reward event; normal purchases and input
        # go through the same director and per-seat dispatch used in play.
        script('for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
               'if(!c.OnSameTeam(ReadCharacterID(0)))ProgressionReportHit(c.GetID(),0,0,true);}')
        wait(lambda: rows()[0]["display_pooled"] >= 1, "campaign kills fill purple pool")
        script("ProgressionBeginShop(false);")
        wait(lambda: all(r.get("shop") and r.get("focus") for r in rows()), "both independent shops open")
        wait(lambda: rows()[0]["display_held"] > 0 and rows()[0]["display_pooled"] < .01, "commit animation transfers XP")
        baseline = rows()
        assert all(not row["columns"] and "restore_health" in row["visible_cards"] for row in baseline)
        before = baseline[1]["display_held"]
        assert baseline[0]["focus"] == baseline[1]["focus"] == "bone_health", baseline
        # Test the actual SDL right-stick map, independent of the camera binding.
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=29000)
        wait(lambda: rows()[1]["scroll"] > 3, "P2 right stick scrolls their own shop")
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=-29000)
        wait(lambda: rows()[1]["scroll"] < 1, "P2 reverse scroll reaches top")
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=0)
        assert rows()[0]["scroll"] == 0
        bridge.call("virtual_pad", action="axis", slot=0, axis=0, value=30000)
        wait(lambda: rows()[1]["focus"] == "flesh_health", "P2 moves one card right")
        time.sleep(.9)
        assert rows()[1]["focus"] == "flesh_health", "one held tilt moved multiple cards"
        bridge.call("virtual_pad", action="axis", slot=0, axis=0, value=0)
        assert rows()[0]["focus"] == "bone_health"
        pad(0)
        wait(lambda: rows()[1]["display_held"] < before, "controller purchase debits only P2")
        assert rows()[0]["display_held"] >= before
        ctx.shot(host, "independent_upgrade_cards")
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=32500)
        wait(lambda: "armor_3" in rows()[1].get("visible_cards", []), "P2 reaches wooden guard previews")
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=0)
        ctx.shot(host, "stock_armor_catalog")
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=29000)
        wait(lambda: "staff" in rows()[1].get("visible_cards", []), "P2 reaches weapon previews")
        time.sleep(.4)
        bridge.call("virtual_pad", action="axis", slot=0, axis=3, value=0)
        ctx.shot(host, "equipment_rarities")
        p2 = baseline[1]["actor"]
        body = next(player["avatar_id"] for player in bridge.status()["online"]["players"] if player["id"] == p2)
        script("permanent_health=.7f;temp_health=.7f;blood_health=.8f;", body)
        wait(lambda: any(c["id"] == "restore_health" and c["available"] for c in
                         bridge.call("progression", actor=p2)["view"]["cards"]), "P2 has an injury to heal")
        bridge.call("virtual_pad", action="axis", slot=0, axis=4, value=32767)
        wait(lambda: next(c["permanent_health"] for c in bridge.state() if c["id"] == body) >= .999,
             "P2 left trigger heals the selected fighter")
        bridge.call("virtual_pad", action="axis", slot=0, axis=4, value=-32768)
        bridge.call("virtual_pad", action="axis", slot=0, axis=5, value=32767)
        wait(lambda: bridge.call("progression", actor=p2)["view"]["ready"], "P2 right trigger readies only P2")
        time.sleep(.35)
        assert bridge.call("progression", actor=p2)["view"]["ready"]
        assert not bridge.call("progression", actor=baseline[0]["actor"])["view"]["ready"]
        bridge.call("virtual_pad", action="axis", slot=0, axis=5, value=-32768)
        press(0,"menu_cancel")
        press(0,"menu_return")
        wait(lambda: all(not r["shop"] for r in rows()), "ready seats leave shop")
        script("LobbyEndGame();")
        wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "back in lobby")
        bridge.call("menu", script="multiplayer_lobby.as")
        wait(lambda: host_form().get("max_scroll_y",0) > 0, "native lobby form overflows")
        script('ImGui_SetWindowFocus("Hosting options");', "menu")
        stick(30000)
        time.sleep(.4)
        assert host_form().get("scroll_y",0) < 1, "nonleader right stick moved the host lobby"
        stick(0)
        script('SetControllerPlayer(0);for(uint i=0;i<LobbyPlayers().length();++i)LobbySetPlayerTeam(LobbyPlayers()[i].id,1);'
               'LobbySetModeSetting("progression_shared",1);LobbyStart();', "menu")
        _ready(host,FIRST)
        dialogue_controller = int(_probe(host, "GetCampaignDialogueControllerID()"))
        _skip(host, dialogue_controller)
        script("ProgressionBeginShop(true);")
        wait(lambda: len(rows()) == 1 and rows()[0]["shop"], "shared shop")
        assert [r["page"] for r in rows()] == ["shop"], rows()
        ctx.shot(host, "host_shared_shop")
        assert not script_errors(host), script_errors(host)
        return {"passed": True, "metrics": {"independent": baseline, "shared": rows()}, "notes": []}
    finally:
        host.stop()
