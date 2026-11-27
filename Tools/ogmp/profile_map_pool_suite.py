"""Map-start upgrade pool across profiles, peers, purchases, hands and saves."""
import json
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows, _skip
from .scenarios import script_errors


def profile_map_pool(ctx):
    bundle = "og-online-progression"
    host = ctx.instance("pool_host", width=1280, height=720, keys={**RENDER_KEYS, "mp_port": "29682"})
    guest = ctx.instance("pool_guest", width=800, height=450, keys={**RENDER_KEYS, "mp_port": "29683"})
    initial = {"Turner": {"health", "armor_3:1", "armor_3:2"},
               "Raider": {"damage", "dog_knife", "ai:2"}, "Bystander": {"speed", "recruit"},
               "Healer": {"restore_health"}}
    for name in initial:
        assert _probe(host, f'ProfileEdit("create","{name}","","")') == "true"
    assert _probe(host, f'ProfileSetModSetting("{bundle}","upgrade_progression",1)') == "true"
    # Seed specific higher-tier prizes without making the test depend on lucky dice.
    host.stop()
    archive = Path(host.writedir, "Data/player_profiles.json")
    stored = json.loads(archive.read_text())
    for name, collection in initial.items():
        stored["profiles"][name]["mods"][bundle]["upgrades"]["owned"] = dict.fromkeys(collection, True)
    stored["profiles"]["Turner"]["mods"][bundle]["upgrades"]["owned"]["absent-mod.axe"] = True
    archive.write_text(json.dumps(stored))
    host.start(reuse_profile=True)

    def wait(fn, label):
        return host.bridge.wait(fn, 45, step=.1, what=label)

    def data(peer, player, **args):
        return peer.bridge.call("progression", actor=player, **args)

    def card_keys(peer, player, **args):
        view = data(peer, player, **args)["view"]
        return {c["id"] for c in view.get("cards", [])}

    def panel(peer, player):
        return next((row for row in peer.bridge.status()["progression_ui"] or [] if row["actor"] == player), {})

    def start():
        _code(host, "LobbyStart();")
        for peer in (host, guest):
            _ready(peer, FIRST)

    def lobby():
        wait(lambda: all(peer.bridge.status()["online"]["lobby_waiting"] and
                         not peer.bridge.status()["level_loaded"] for peer in (host, guest)), "peers in lobby")

    def restore(slot):
        _code(host, f"SessionLoad({slot});", "level")
        lobby()
        start()
        wait(lambda: data(host, owner)["view"].get("shop_open"), "shop from the saved map")

    assert _probe(host, "LobbyBeginHost()") == "true"
    guest.bridge.call("connect", ip="127.0.0.1:29682")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "two human seats")
    owner = next(p["id"] for p in _rows(host) if p["host"])
    visitor = next(p["id"] for p in _rows(host) if not p["host"])
    assert _probe(host, f'LobbyProfileEdit({owner},"select","","Turner")') == "true"
    assert _probe(guest, f'LobbyProfileEdit({visitor},"select","","Raider")') == "true"
    wait(lambda: any(p["name"] == "Raider" for p in _rows(host)), "host applied guest selection")
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);'
          'LobbySetModeSetting("progression_timer",0);LobbySetModeSetting("progression_free_buffs",1);'
          'LobbySetModeSetting("progression_gain_checkpoint",1);LobbyAddBot();')
    bot = next(p["id"] for p in _rows(host) if p["bot"])
    start()
    _skip(host)
    # Raider predates the split between blunt trauma and flesh damage. Its one old
    # unlock grants both categories, and the host writes the migrated collection.
    shared = initial["Turner"] | (initial["Raider"] - {"damage"}) | {"bone_damage", "flesh_damage"}
    migrated_owned = json.loads(archive.read_text())["profiles"]["Raider"]["mods"][bundle]["upgrades"]["owned"]
    assert "damage" not in migrated_owned and migrated_owned["bone_damage"] and migrated_owned["flesh_damage"]
    for peer, player in ((host, owner), (guest, visitor)):
        wait(lambda: set(data(peer, player)["ledger"].get("map_upgrades", [])) == shared, "shared starting pool")
    # Enough ordinary checkpoint XP to prove that an unavailable purchase is refused for eligibility.
    _code(host, "for(int i=1;i<=70;++i) ProgressionCheckpoint(i,483);ProgressionBeginShop(false);", "level")
    expected = {"health", "bone_damage", "flesh_damage", "armor_3", "dog_knife"}
    for peer, player in ((host, owner), (guest, visitor)):
        wait(lambda: card_keys(peer, player) == expected, "both shops expose the union")
        view = data(peer, player)["view"]
        assert view["held"] > 1000
        assert view["reward_cards"] and {c["id"] for c in view["reward_cards"]} <= expected | {"ai"}
        wait(lambda: panel(peer, player).get("visible_cards"), "populated upgrade display")
        assert not panel(peer, player).get("empty_upgrades")
    assert card_keys(host, owner, target=bot) == {"armor_3", "dog_knife", "ai"}
    assert not data(host, owner, action="buy", key="speed")["submitted"]
    for material in (1, 2):
        armor = next(c for c in data(host, owner)["view"]["cards"] if c["id"] == "armor_3")
        assert armor["armor_material"] == material
        assert data(host, owner, action="buy", key="armor_3")["submitted"]
    assert "armor_3" not in card_keys(host, owner)
    # Remove one contributor and switch off progression while in-game. The
    # map still honors both players' original upgrade contributions.
    _code(host, f'ProfileEdit("delete","Raider","","");ProfileSetModSetting("{bundle}","upgrade_progression",0);', "level")
    assert card_keys(host, owner) == expected - {"armor_3"}
    assert card_keys(guest, visitor) == expected
    _code(host, "SessionSave(1);", "level")
    card = Path(host.writedir, "Data/SessionSaves/slot1.json")
    wait(card.exists, "restricted shop saved")
    saved = json.loads(card.read_text())
    assert set(saved["session_mods"][bundle]["runtime"]["map_upgrades"]) == shared
    # A v7 map stored the common damage unlock and a single rank for each
    # individual or shared wallet. Both upgraded channels retain its value.
    before_split = json.loads(card.read_text())
    old_section = before_split["session_mods"][bundle]
    old_section["version"] = "7"
    old_runtime = old_section["runtime"]
    old_runtime["map_upgrades"] = sorted((shared - {"bone_damage", "flesh_damage"}) | {"damage"})
    for row in old_runtime["players"] + old_runtime["teams"]:
        row["damage"] = row.pop("bone_damage")
        del row["flesh_damage"], row["fall_resistance"]
    card.write_text(json.dumps(before_split))
    restore(1)
    wait(lambda: card_keys(host, owner) == expected - {"armor_3"} and card_keys(guest, visitor) == expected,
         "restore preserves the original union after profile deletion")
    # A real v6 card has no map-wide restriction. Keep its already open shop fully usable.
    old = saved["session_mods"][bundle]
    del old["runtime"]["map_upgrades"]
    old["version"] = "6"
    for row in old["runtime"]["players"] + old["runtime"]["teams"]:
        row["damage"] = 3 if row.get("id") == owner else 0
        del row["bone_damage"], row["flesh_damage"], row["fall_resistance"]
    card.write_text(json.dumps(saved))
    restore(1)
    wait(lambda: {"speed", "recruit", "dagger"} <= card_keys(host, owner), "unrestricted legacy shop")
    stats = data(host, owner)["view"]["stats"]
    assert stats["bone_damage_rank"] == stats["flesh_damage_rank"] == 3 and stats["fall_resistance_rank"] == 0
    _code(host, "LobbyEndGame();", "level")
    lobby()
    assert _probe(host, f'ProfileSetModSetting("{bundle}","upgrade_progression",1)') == "true"
    assert _probe(host, f'LobbyProfileEdit({owner},"select","","")') == "true"
    start()
    _skip(host)
    _code(host, "ProgressionBeginShop(false);", "level")
    for peer, seat in ((host, owner), (guest, visitor)):
        wait(lambda: data(peer, seat)["view"].get("shop_open"), "empty profile shop")
        assert data(peer, seat)["ledger"]["map_upgrades"] == [] and not card_keys(peer, seat)
        wait(lambda: panel(peer, seat).get("empty_upgrades"), "visible directions for an empty map pool")
        ui = panel(peer, seat)
        assert all(detail in ui["empty_upgrades"] for detail in
                   ("No upgrades unlocked", "10 campaign clears", "10 Arena match wins", "10 Survival batteries",
                    "Roll upgrade", "Profiles", "lobby", "next map"))
        assert ui["columns"] == (peer is host)
        ctx.shot(peer, "empty_upgrades_columns" if peer is host else "empty_upgrades_tab")

    _code(host, "LobbyEndGame();", "level")
    lobby()
    assert _probe(host, f'LobbyProfileEdit({owner},"select","","Healer")') == "true"
    _code(host, 'LobbySetModeSetting("progression_shared",1);')
    start()
    _skip(host)
    _code(host, "ProgressionBeginShop(false);", "level")
    for peer, seat in ((host, owner), (guest, visitor)):
        wait(lambda: data(peer, seat)["view"].get("shop_open") and panel(peer, seat).get("shop"), "shared shop")
    leader = data(host, owner)["view"]["team_leader"]
    for peer, seat in ((host, owner), (guest, visitor)):
        if seat == leader:
            wait(lambda: "Use Heal above" in panel(peer, seat).get("empty_upgrades", ""), "heal-only upgrade display")
            assert "Roll upgrade" in panel(peer, seat)["empty_upgrades"]
            assert card_keys(peer, seat) == {"restore_health"} and "restore_health" in panel(peer, seat)["action_rects"]
        else:
            assert not data(peer, seat)["view"]["can_buy"] and not panel(peer, seat).get("empty_upgrades")
    assert not script_errors(host) and not script_errors(guest)
    return {"passed": True, "metrics": {"pooled_unlocks": len(shared), "armor_tiers_bought": 2,
                                        "legacy_shop_restored": True, "empty_guest_pool": True,
                                        "wide_and_tabbed_guidance": True, "heal_only_display": True}, "notes": []}
