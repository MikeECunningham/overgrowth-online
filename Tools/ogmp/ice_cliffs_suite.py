"""Carry a ship-shop loadout into Ice Cliffs, fall through its death zone and retry at the shelf."""
import math
import time

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, _code, _probe, _ready, _skip
from .scenarios import script_errors

LEVEL = "og_story/08_Ice_Cliff_Landing.xml"
SHIP = "og_story/07_Beach_Boat_Assault.xml"
CHECKPOINTS = (309, 310, 311, 313, 314)


def ice_cliffs_retries(ctx):
    game = ctx.instance("ice_cliffs_retries", width=960, height=540, keys={
        **RENDER_KEYS, "player_invincible": "false", "mp_port": "26993",
        "difficulty_scaling": "true", "campaign_mp_scaling": "true", "armor_localized": "2"})
    bridge = game.bridge
    _code(game, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{SHIP}",false);'
          'LobbySetModeSetting("progression",1);LobbyStart();', "menu")
    _ready(game, SHIP)
    _skip(game)
    players = bridge.status()["online"]["players"]
    assert len(players) == 1 and not players[0]["bot"], players
    player = players[0]["id"]

    # Bank encounter rewards on the ship. The chapter victory and the shop's
    # Ready action then take the regular campaign path into Ice Cliffs.
    sailor = bridge.me()["id"]
    _code(game, f'for(int k=0;k<8;++k){{for(int i=0;i<GetNumCharacters();++i){{'
          f'MovementObject@ e=ReadCharacter(i);if(!e.OnSameTeam(ReadCharacterID({sailor})))'
          f'ProgressionReportHit(e.GetID(),{sailor},0,true);}}'
          'ProgressionCheckpoint();ProgressionResetAttempt();} SendGlobalMessage("levelwin");')
    bridge.wait(lambda: bridge.call("progression", actor=player)["view"].get("shop_open"),
                15, what="ship victory opens shop")
    for upgrade in ("dagger", "bone_health", "armor_3"):
        assert bridge.call("progression", actor=player, action="buy", key=upgrade)["submitted"], upgrade
    assert bridge.call("progression", actor=player, action="ready")["submitted"]
    _ready(game, LEVEL)
    _skip(game)
    actor = bridge.me()["id"]

    def loadout():
        world = bridge.call("state")
        character = next(c for c in world["characters"] if c["id"] == actor)
        weapons = sorted(item["file"].split("/")[-1].lower() for item in world["items"]
                         if item["id"] in character["weapon_slots"][:4])
        armor = sorted(piece["path"] for piece in character["armor_attachments"])
        return weapons, armor

    def has_purchases():
        weapons, armor = loadout()
        return "rabbit_knife.xml" in weapons and any("large_wood_plate" in path for path in armor)
    bridge.wait(has_purchases, 15, what="purchased dagger and wooden guard appear on Ice Cliffs")
    initial_loadout = loadout()

    def checkpoint(index, marker):
        bridge.call("level_messages", prefix="ohl og_progress")
        _code(game, f'MovementObject@ c=ReadCharacterID({actor});c.static_char=true;'
              f'c.position=ReadObjectFromID({marker}).GetTranslation();c.velocity=vec3(0);'
              'c.Execute("FixDiscontinuity();");')
        def reached():
            messages = bridge.call("level_messages", prefix="ohl og_progress")["messages"]
            return any(int(m.split()[2]) == index + 1 for m in messages)
        bridge.wait(reached, 12, what=f"authored reach goal {index}")

    def retry():
        bridge.wait(lambda: bridge.me()["knocked_out"] != 0, 18, what="fall knocks the human out")
        def restart():
            if bridge.me()["knocked_out"] == 0:
                return True
            bridge.call("input", controller=0, hold={"attack": 1}, steps=10)
            time.sleep(.18)
            return False
        bridge.wait(restart, 12, what="spectator restarts at checkpoint")
        assert _probe(game, f'ReadCharacterID({actor}).GetBoolVar("fall_death")') == 'false'
        bridge.wait(lambda: loadout() == initial_loadout, 10, what="checkpoint restores purchased equipment")

    # First retry before heading upriver, matching the preceding equipment reset.
    checkpoint(0, CHECKPOINTS[0])
    _code(game, f'ReadCharacterID({actor}).Execute("invincible=false;ko_shield=0;TakeDamage(8);");')
    retry()
    for index, marker in enumerate(CHECKPOINTS[1:], 1):
        checkpoint(index, marker)
    saved = [float(_probe(game, f'ReadObjectFromID(338).GetTranslation().{axis}')) for axis in 'xyz']
    landings = []
    for _ in range(4):
        _code(game, f'MovementObject@ c=ReadCharacterID({actor});c.static_char=false;'
              'c.position=vec3(124.927,68.4,-32.4052);c.velocity=vec3(0,-16,0);'
              'c.Execute("SetOnGround(false);FixDiscontinuity();");')
        bridge.wait(lambda: _probe(game, f'ReadCharacterID({actor}).GetBoolVar("fall_death")') == 'true',
                    8, step=.05, what="cross live death volume")
        retry()
        pos = bridge.me()["pos"]
        assert math.dist(pos, saved) < 3, (saved, pos)
        landings.append(pos)
        time.sleep(.5)
    assert not script_errors(game), script_errors(game)
    return {"passed": True, "notes": [], "metrics": {
        "actor": actor, "checkpoint": saved, "respawns": landings, "restored_loadout": initial_loadout}}
