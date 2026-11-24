"""Preserve stock composite suits while arbitrating duplicate and purchased armor."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _probe, _ready, _rows, _skip
from .scenarios import script_errors


def authored_armor_slots(ctx):
    host = ctx.instance("authored_armor_host", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28651", "difficulty_scaling": "false", "armor_localized": "2",
        "player_invincible": "true", "tutorials": "false"})
    guest = ctx.instance("authored_armor_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "28652"})
    peers = [host, guest]
    wait = lambda fn, label: host.bridge.wait(fn, 35, step=.12, what=label)
    _code(host, 'LobbyBeginHost();', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:28651")
    wait(lambda: all(len(_rows(p)) == 2 for p in peers), "two armor owners")
    p1 = next(p["id"] for p in _rows(host) if p["host"])
    p2 = next(p["id"] for p in _rows(host) if not p["host"])

    def start(progression, campaign=CAMPAIGN, level="Data/Levels/"+FIRST):
        _code(host, f'LobbySetSelection("{campaign}","{campaign}","{level}",false);'
              f'LobbySetModeSetting("progression",{progression});LobbyStart();', "menu")
        for peer in peers:
            _ready(peer, level)
        _skip(host)
        for peer in peers:
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                  'if(Online_IsPlayerCharacter(c.GetID())){if(!c.network_proxy)c.static_char=true;}'
                  'else c.Execute("combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");}')

    def actor(common, peer=host):
        return next((c for c in peer.bridge.state() if c["common_id"] == common), None)

    def pieces(common, peer=host):
        return actor(common, peer)["armor_attachments"]

    def visible(common, peer=host):
        return {p["common_id"] for p in pieces(common, peer) if p["enabled"]}

    def spawn(path, offset):
        hero = host.bridge.me()
        common = host.bridge.call("spawn", path=path, pos=[hero["pos"][0]+offset, hero["pos"][1], hero["pos"][2]],
                                  teams=hero["teams"])["object_id"]
        wait(lambda: all(actor(common, peer) is not None for peer in peers), "stock armor actor replicated")
        for peer in peers:
            _code(peer, f'ReadObjectFromID({actor(common, peer)["id"]}).SetPlayer(false);'
                  f'MovementObject@ c=ReadCharacterID({actor(common, peer)["id"]});c.ChangeControlScript("enemycontrol.as");c.controlled=false;c.static_char=true;'
                  'c.Execute("combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");')
        return common

    def enabled(common_piece, value):
        for peer in peers:
            found = next((p for c in peer.bridge.state() for p in c["armor_attachments"] if p["common_id"] == common_piece), None)
            if found is None or found["enabled"] != value:
                return False
        return True

    def toggle(common_piece, value):
        _code(host, f'ReadObjectFromID({common_piece}).SetEnabled({str(value).lower()});')
        for peer in peers[1:]:
            found = next(p for c in peer.bridge.state() for p in c["armor_attachments"] if p["common_id"] == common_piece)
            _code(peer, f'ReadObjectFromID({found["id"]}).SetEnabled({str(value).lower()});')

    start(0)
    dog = spawn("Data/Objects/lugaru_campaign/turner_armored.xml", 3)
    chitin = spawn("Data/Prefabs/handofouran.xml", 5)
    dog_original = visible(dog)
    chitin_original = visible(chitin)
    assert sum("shoulder_" in p["path"] for p in pieces(dog) if p["enabled"]) == 4
    assert sum("/chitinarmour/backplate.xml" in p["path"] for p in pieces(chitin) if p["enabled"]) == 5
    wait(lambda: all(visible(dog, p) == dog_original and visible(chitin, p) == chitin_original for p in peers),
         "stock shoulder and segmented back assemblies remain visible")
    chest = next(p for p in pieces(dog) if "breastplate.xml" in p["path"])
    exact = host.bridge.call("armor_fixture", char=dog, op="duplicate", piece=chest["id"], region=1, authored=True)["copy"]
    weaker = host.bridge.call("armor_fixture", char=dog, op="duplicate", piece=chest["id"], region=1,
                              authored=True, path="Data/Objects/therium/chitinarmour/chestplate.xml")["copy"]
    wait(lambda: enabled(exact, False) and enabled(weaker, False) and enabled(chest["common_id"], True),
         "exact and weaker-family overlaps suppressed with progression and scaling off")
    assert all({p["common_id"] for p in pieces(dog, peer)} >= dog_original | {exact, weaker} for peer in peers)
    _code(host, f'DeleteObjectID({exact});')
    toggle(chest["common_id"], False)
    wait(lambda: enabled(weaker, True), "authored alternative restored when the stronger plate is disabled")
    assert enabled(chest["common_id"], False), "arbitration enabled an authored quest-disabled plate"
    toggle(chest["common_id"], True)
    wait(lambda: enabled(weaker, False), "restored authored plate wins its slot again")
    _code(host, 'level.SendMessage("reset");')
    _skip(host)
    wait(lambda: all(visible(dog, peer) == dog_original for peer in peers), "reset preserves original composite gear and suppression")
    _code(host, 'LobbyEndGame();')
    wait(lambda: all(_rows(peer) and peer.bridge.status()["online"]["lobby_waiting"] and
                     not peer.bridge.status()["level_loaded"] for peer in peers), "unloaded lobby")

    _code(host, f'LobbySetForceTeams(true);LobbySetPlayerTeam({p2},2);', "menu")
    start(1, ARENA, ARENA_MAP)
    wait(lambda: all(next(p["avatar_id"] for p in _rows(peer) if p["id"] == p2) >= 0 for peer in peers),
         "opposing Arena human assigned")
    avatar = next(p["avatar_id"] for p in _rows(host) if p["id"] == p2)
    wait(lambda: all(actor(avatar, peer) is not None and actor(avatar, peer)["enabled"] for peer in peers),
         "opposing Arena human ready")
    hero = host.bridge.me()
    native = host.bridge.call("spawn", path="Data/Items/gear/rabbit_gear/large_wood_plate.xml",
                              pos=[hero["pos"][0], hero["pos"][1]+2, hero["pos"][2]])["object_id"]
    host.bridge.call("armor_fixture", char=avatar, op="wear", item=native,
                     slot="Data/Items/attachments/rightlowerarm.xml", mirrored=True)
    wait(lambda: host.bridge.call("armor_fixture", char=avatar, op="inspect")["authored_regions"] & 8,
         "opponent wears authored forearm armor")
    victim = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                              pos=[hero["pos"][0]+8, hero["pos"][1], hero["pos"][2]], teams="armor_reward")["object_id"]
    _code(host, f'for(int n=0;n<100;++n){{ProgressionReportHit({victim},{avatar},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();}ProgressionBeginShop(false);')
    wait(lambda: guest.bridge.call("progression", actor=p2)["view"].get("shop_open"), "opponent's personal shop")
    assert guest.bridge.call("progression", actor=p2, action="buy", key="armor_3")["submitted"]
    assert host.bridge.call("progression", actor=p1, action="ready")["submitted"]
    assert guest.bridge.call("progression", actor=p2, action="ready")["submitted"]

    def purchased_only():
        for peer in peers:
            world = peer.bridge.call("state")
            native_piece = next((i for i in world["items"] if i["common_id"] == native), None)
            if not native_piece or native_piece["enabled"]:
                return False
            local = actor(avatar, peer)
            if not any(p["region"] == 8 and p["enabled"] for p in local["armor_attachments"]):
                return False
            result = peer.bridge.call("armor_fixture", char=local["id"], op="inspect")
            if result["authored_regions"] & 8 or not math.isclose(result["rating"], .065*.5, abs_tol=1e-5):
                return False
        return True

    wait(purchased_only, "opponent's purchased forearm replaces its authored wearable on both peers")
    _code(host, 'level.SendMessage("reset");')
    _skip(host)
    wait(purchased_only, "opponent retry restores one active armor item per slot")
    assert all(not script_errors(peer) for peer in peers), [script_errors(peer) for peer in peers]
    return {"passed": True, "notes": [], "metrics": {"dog_components": len(dog_original),
            "chitin_components": len(chitin_original), "duplicates_preserved_but_hidden": [exact, weaker],
            "opponent_armor_rating": .065*.5}}
