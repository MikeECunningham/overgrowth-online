"""Host disk saves, card thumbnails, Continue and an editable restored lobby."""

import json
import shutil
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _probe, _ready, _skip
from .scenarios import script_errors


def session_saves(ctx):
    host = ctx.instance("save_cards_host", keys={**RENDER_KEYS, "mp_port": "28684"})
    guest = ctx.instance("save_cards_guest", keys={**RENDER_KEYS, "mp_port": "28685"})

    def menu(inst, code):
        _code(inst, code, "menu")

    def wait(inst, fn, message):
        return inst.bridge.wait(fn, 45, what=message)

    def players(inst):
        return inst.bridge.status()["online"]["players"]

    def source(inst, slot):
        return Path(inst.writedir, "Data", "SessionSaves", f"slot{slot}.json")

    def button(inst, key):
        inst.bridge.call("key", key=key, down=True)
        time.sleep(0.12)
        inst.bridge.call("key", key=key, down=False)

    menu(host, "LobbyBeginHost();LobbyAddBot();")
    guest.bridge.call("connect", ip="127.0.0.1:28684")
    wait(host, lambda: len(players(host)) == 3, "two humans and a bot")
    menu(
        host,
        f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
        'LobbySetModeSetting("progression",1);LobbySetModeSetting("challenge_round",4);'
        "LobbySetAllowAllPause(true);LobbyStart();",
    )
    _ready(host, FIRST)
    _ready(guest, FIRST)
    _skip(host)
    wait(host, lambda: source(host, 0).exists(), "first camera autosave")
    initial = json.loads(source(host, 0).read_text())
    assert initial["lobby"] and initial["rules"]["allow_pause"]
    assert not source(guest, 0).exists()
    hero = host.bridge.me()["id"]
    _code(host, f"ProgressionReportHit(363,{hero},0,true);ProgressionCheckpoint();")
    _code(host, f'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint 0 {hero} 477");')
    wait(
        host,
        lambda: json.loads(source(host, 0).read_text())["world"]
        .get("goals", {})
        .get("14", "")
        .split()[:1]
        == ["1"],
        "story checkpoint autosaves its objective",
    )
    # Only the manual card will contain this purchase. Continue and Load must
    # select distinct records even though they point at the same checkpoint.
    host_slot = next(p["id"] for p in players(host) if p["host"])
    guest_slot = next(p["id"] for p in players(host) if not p["bot"] and not p["host"])
    _code(host, "ProgressionBeginShop(false);")
    wait(
        host,
        lambda: host.bridge.call("progression", actor=host_slot)["view"].get("shop_open"),
        "campaign shop open",
    )
    assert host.bridge.call("progression", actor=host_slot, action="buy", key="health")["submitted"]
    wait(
        host,
        lambda: host.bridge.call("progression", actor=host_slot)["view"]["stats"]["health_rank"] == 1,
        "host slot buys durability",
    )
    for peer, slot in [(host, host_slot), (guest, guest_slot)]:
        assert peer.bridge.call("progression", actor=slot, action="ready", key="")["submitted"]
    wait(
        host,
        lambda: not host.bridge.call("progression", actor=host_slot)["view"].get("shop_open"),
        "purchased upgrade committed",
    )
    _code(host, f'ReadCharacterID({hero}).SetFloatVar("permanent_health",0.625f);')
    button(host, "Escape")
    wait(host, lambda: bool(host.bridge.status()["open_menu_players"]), "host pause menu")
    _code(host, 'imGUI.receiveMessage(IMMessage("open_save_cards"));', "player_menu")
    _code(host, 'imGUI.receiveMessage(IMMessage("choose_save_card",1));', "player_menu")
    wait(host, lambda: source(host, 1).exists(), "manual card saved from the pause menu")
    record = json.loads(source(host, 1).read_text())
    thumb = Path(host.writedir, "Data/Textures/SessionSaves", f'slot1-{record["image_id"]}.jpg')
    from PIL import Image

    with Image.open(thumb) as picture:
        assert picture.size == (256, 144) and max(hi - lo for lo, hi in picture.getextrema()) > 20
    ctx.shot(host, "pause_save_cards")
    assert not script_errors(host), script_errors(host)
    # A fresh process gets only the host's disk saves. It has no old roster,
    # config, network connections, or in-memory progression to fall back on.
    cold = ctx.instance("save_cards_cold", keys={**RENDER_KEYS, "mp_port": "28686"})
    for folder in ("SessionSaves", "Textures/SessionSaves"):
        shutil.copytree(
            Path(host.writedir, "Data", folder), Path(cold.writedir, "Data", folder), dirs_exist_ok=True
        )
    menu(cold, 'imGUI.receiveMessage(IMMessage("Continue"));')
    wait(
        cold,
        lambda: cold.bridge.status()["online"].get("lobby_waiting") and len(players(cold)) >= 2,
        "Continue opens the saved lobby",
    )
    menu(cold, 'this_ui.SendCallback("session_load_menu.as");')
    time.sleep(0.6)
    ctx.shot(cold, "load_cards")
    menu(cold, 'imGUI.receiveMessage(IMMessage("choose_save_card",1));')
    wait(
        cold,
        lambda: cold.bridge.status()["online"].get("lobby_waiting"),
        "manual card opens saved lobby",
    )
    time.sleep(0.8)
    menu(cold, "LobbyStart();")
    _ready(cold, FIRST)
    wait(
        cold,
        lambda: _probe(cold, 'cast<Hotspot@>(ReadObjectFromID(14)).GetIntVar("progress")') == "1",
        "saved objective restored",
    )
    assert _probe(cold, 'LobbyGetModeSetting("progression")') == "1"
    assert _probe(cold, 'LobbyGetModeSetting("challenge_round")') == "4"
    assert _probe(cold, "LobbyGetAllowAllPause()") == "true"
    owner = next(p["id"] for p in players(cold) if p["host"])
    saved_player = next(p for p in record["progression"]["players"] if p["id"] == owner)
    view = cold.bridge.call("progression", actor=owner)["view"]
    assert view["held"] == saved_player["w"]["held"] and view["held"] > 0, (view, saved_player)
    assert view["stats"]["health_rank"] == 1 and saved_player["health"] == 1
    time.sleep(1.4)
    assert not cold.bridge.status()["dialogue_camera_control"]
    assert (
        abs(
            float(
                _probe(
                    cold,
                    "ReadCharacterID("
                    + str(cold.bridge.me()["id"])
                    + ').GetFloatVar("permanent_health")',
                )
            )
            - 0.625
        )
        < 0.005
    )
    # A long-running campaign can exceed both the former 100M ceiling and the
    # old 32-bit HUD conversion. Start with a large saved balance, then earn
    # and spend through the normal campaign and shop paths.
    large_held = 2**32 + 15
    large_card = json.loads(source(cold, 1).read_text())
    next(p for p in large_card["progression"]["players"] if p["id"] == owner)["w"]["held"] = large_held
    source(cold, 2).write_text(json.dumps(large_card))
    _code(cold, "SessionLoad(2);")
    wait(cold, lambda: cold.bridge.status()["online"]["lobby_waiting"], "high-balance card opens lobby")
    wait(cold, lambda: any(p["id"] == owner and p["w"]["held"] == large_held
                           for p in cold.bridge.call("progression", actor=owner)["ledger"]["players"]),
         "saved unit assigned in restored lobby")
    menu(cold, "LobbyStart();")
    _ready(cold, FIRST)
    wait(cold, lambda: cold.bridge.call("progression", actor=owner)["view"].get("held") == large_held,
         "large balance restored")
    _code(cold, f'ProgressionReportHit(363,{cold.bridge.me()["id"]},0,true);ProgressionCheckpoint();'
                'ProgressionBeginShop(false);')
    wait(cold, lambda: cold.bridge.call("progression", actor=owner)["view"]["held"] > large_held,
         "earn XP above the old ceiling")
    shop = cold.bridge.call("progression", actor=owner)["view"]
    cost = next(card["cost"] for card in shop["cards"] if card["id"] == "damage")
    assert cold.bridge.call("progression", actor=owner, action="buy", key="damage")["submitted"]
    remaining = shop["held"] - cost
    wait(cold, lambda: any(row["display_held"] == remaining for row in cold.bridge.status()["progression_ui"]),
         "HUD shows full balance after purchase")
    ledger = cold.bridge.call("progression", actor=owner)["ledger"]
    assert next(p for p in ledger["players"] if p["id"] == owner)["w"]["held"] == remaining
    assert not script_errors(cold), script_errors(cold)
    return {
        "passed": True,
        "metrics": {
            "mode": record["mode_name"],
            "map": record["map_name"],
            "thumbnail": [256, 144],
            "saved_slots": len(record["slots"]["slots"]),
            "restored_xp": view["held"],
            "restored_durability_rank": view["stats"]["health_rank"],
            "restored_large_balance": large_held,
            "large_balance_after_purchase": remaining,
        },
        "notes": [],
    }


def session_saves_solo(ctx):
    """Offline campaign saves use the pause gallery, including replacing a card."""

    def key(inst, name):
        for down in (True, False):
            inst.bridge.call("key", key=name, down=down)
            time.sleep(0.12)

    def choose(inst, name):
        _code(inst, 'imGUI.receiveMessage(IMMessage("' + name + '"));', "player_menu")
        time.sleep(0.15)

    def card(inst, slot):
        return Path(inst.writedir, "Data", "SessionSaves", f"slot{slot}.json")

    solo = ctx.instance("save_cards_solo", keys={**RENDER_KEYS, "difficulty_scaling": "false"})
    solo.bridge.call("load_level", path="set_campaign " + CAMPAIGN)
    solo.bridge.call("load_level", path="load_campaign_level river_village_intro")
    _ready(solo, FIRST)
    _skip(solo)
    assert not solo.bridge.status()["online"]["active"]
    solo.bridge.wait(lambda: card(solo, 0).exists(), 15, what="offline autosave")
    key(solo, "Escape")
    solo.bridge.wait(lambda: bool(solo.bridge.status()["open_menu_players"]), 10, what="offline pause")
    choose(solo, "open_save_cards")
    select = 'imGUI.receiveMessage(IMMessage("choose_save_card",2));'
    _code(solo, select, "player_menu")
    solo.bridge.wait(lambda: card(solo, 2).exists(), 10, what="offline manual save")
    first = json.loads(card(solo, 2).read_text())
    assert not first["lobby"]
    _code(solo, select, "player_menu")
    choose(solo, "cancel_save")
    assert json.loads(card(solo, 2).read_text()) == first
    _code(solo, select, "player_menu")
    choose(solo, "confirm_save")
    solo.bridge.wait(
        lambda: json.loads(card(solo, 2).read_text())["image_id"] != first["image_id"],
        10,
        what="replacement card",
    )
    record = json.loads(card(solo, 2).read_text())
    assert not Path(
        solo.writedir, "Data/Textures/SessionSaves", f'slot2-{first["image_id"]}.jpg'
    ).exists()
    # A structurally damaged card leaves the current scene and valid cards intact.
    damaged = {**record, "world": {**record["world"], "round": "broken"}}
    card(solo, 3).write_text(json.dumps(damaged))
    _code(solo, 'SetConfigValueBool("test_bad_save_accepted", SessionLoad(3));', "player_menu")
    assert _probe(solo, 'GetConfigValueBool("test_bad_save_accepted")') == "false"
    assert card(solo, 2).exists() and solo.bridge.status()["level_loaded"]
    # Change the current unit, then load the manual card through the pause menu.
    actor = solo.bridge.me()["id"]
    _code(solo, f'ReadCharacterID({actor}).SetFloatVar("permanent_health",0.25f);')
    choose(solo, "save_back")
    choose(solo, "open_load_cards")
    _code(solo, select, "player_menu")
    solo.bridge.wait(
        lambda: not solo.bridge.status()["open_menu_players"], 25, what="pause load replaces scene"
    )
    _ready(solo, FIRST)
    saved = next(v for k, v in record["world"]["actors"].items() if k.startswith("solo"))

    def health_restored():
        actor = solo.bridge.me()["id"]
        return (
            abs(
                float(_probe(solo, f'ReadCharacterID({actor}).GetFloatVar("permanent_health")'))
                - saved["permanent_health"]
            )
            < 0.001
        )

    solo.bridge.wait(health_restored, 20, what="offline unit restored")
    assert not solo.bridge.status()["online"]["active"]
    assert not script_errors(solo), script_errors(solo)
    return {
        "passed": True,
        "metrics": {
            "mode": record["mode_name"],
            "map": record["map_name"],
            "replaced_card": 2,
            "hosted": False,
        },
        "notes": [],
    }
