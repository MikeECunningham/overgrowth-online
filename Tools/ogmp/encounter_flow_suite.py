"""Authored ship checkpoint recovery and the lobby's automatic Arena battle roster."""
import time

from .newcampaign_local_suite import _ready, _skip
from .scenarios import script_errors


def _probe(inst, expression, arena=False):
    prefix = "encounter_flow_probe"
    inst.bridge.call("level_messages", prefix=prefix)
    inst.bridge.call("exec", target="level", code=f'level.SendMessage("{prefix} " + ({expression}));',
                     **({"context": "level_specific"} if arena else {}))
    messages = inst.bridge.call("level_messages", prefix=prefix)["messages"]
    assert messages, expression
    return messages[-1].split(" ", 1)[1]


def _start(inst, mode, path, guest=None):
    inst.bridge.call("exec", target="menu", code="LobbyBeginHost(); LobbySetForceTeams(false);")
    if guest:
        guest.bridge.call("connect", ip="127.0.0.1:" + inst.keys["mp_port"])
        for peer in (inst, guest):
            peer.bridge.wait(lambda: len(peer.bridge.status()["online"]["players"]) == 2, 35,
                             what="remote joins lobby")
    inst.bridge.call("exec", target="menu", code=(
        f'LobbySetSelection("{mode}","{mode}",'
        f'"Data/Levels/{path}",false); LobbyStart();'))
    _ready(inst, path)
    if guest:
        _ready(guest, path)


def arena_autofill(ctx):
    host = ctx.instance("arena_autofill_host", keys={"mp_port": "28391", "oo_arena_random": "false"})
    _start(host, "com-wolfire-arena", "arenas/Magma_Arena.xml")

    def sizes():
        return [int(x) for x in _probe(host, '"" + int(phase) + " " + roster.length() + " " + '
                                       'fill_bots.length() + " " + TeamCount(roster)', arena=True).split()]

    def check(seats):
        host.bridge.wait(lambda: sizes()[0] == 2, 45, what="Arena opens a playable round")
        _, fighters, temporary, teams = sizes()
        assert 2 <= fighters <= 4 and fighters == seats + temporary and teams >= 2, sizes()
        info = host.bridge.status()
        assert len(info["online"]["players"]) == seats, info["online"]
        assert info["local_players"] == info["render_views"] == 1
        actors = [c for c in host.bridge.state() if c["id"] not in {p["avatar_id"] for p in info["online"]["players"]}]
        assert len(actors) == temporary and all(not c["controlled"] and not c["is_player"] for c in actors)
        return fighters

    samples = [check(1)]
    host.bridge.call("exec", target="level", code="LobbyAddBot();")
    host.bridge.wait(lambda: any(p["bot"] and p["avatar_id"] >= 0 for p in host.bridge.status()["online"]["players"]),
                     25, what="explicit bot has an avatar")
    host.bridge.call("exec", target="level", context="level_specific", code="NewMatch();")
    samples.append(check(2))
    host.bridge.call("exec", target="level", code="LobbyEndGame();")
    host.bridge.wait(lambda: not host.bridge.status()["level_loaded"], 35, what="Arena returns to lobby")
    assert len(host.bridge.status()["online"]["players"]) == 2
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"battle_sizes": samples}, "notes": []}


def ship_checkpoint(ctx):
    host = ctx.instance("ship_checkpoint_host", keys={"mp_port": "28392", "player_invincible": "true",
                                                      "campaign_mp_scaling": "false", "difficulty_scaling": "false"})
    guest = ctx.instance("ship_checkpoint_guest", keys={"mp_port": "28392"})
    _start(host, "com-wolfire-overgrowth-campaign", "og_story/07_Beach_Boat_Assault.xml", guest)
    _skip(host)
    hotspot = 'cast<Hotspot@>(ReadObjectFromID(4110))'
    hero = host.bridge.status()["campaign_main_avatar"]
    assert hero >= 0

    def code(statement, target="level"):
        host.bridge.call("exec", target=target, code=statement)

    def story(variable):
        return int(_probe(host, f'{hotspot}.GetIntVar("{variable}")'))

    def enabled(actor, peer=host):
        return _probe(peer, f'ReadObjectFromID({actor}).GetEnabled()') == "true"

    def defeat(actor):
        code("SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);", actor)

    # The two optional guards may be bypassed. Incidental hostile 4150 is not
    # named in any objective and must stay down after the dock checkpoint.
    defeat(4150)
    code(f'ReadObjectFromID(4110).ReceiveScriptMessage("player_entered_checkpoint 1 {hero} 191");')
    host.bridge.wait(lambda: story("progress") == 2, 35, what="dock checkpoint reached")
    assert not enabled(4155) and not enabled(4156)
    code('level.SendMessage("reset");')
    for peer in (host, guest):
        peer.bridge.wait(lambda: not enabled(4150, peer), 25, what="cleared village guard stays down on reset")
    _skip(host)
    code(f'MovementObject@ hero=ReadCharacterID({hero}); MovementObject@ guard=ReadCharacterID(4143);'
         'hero.position=Online_SafeSpotNear(guard.position,-guard.GetFacing(),24);'
         'hero.Execute("FixDiscontinuity();");')
    assert _probe(host, 'ReadCharacterID(4143).QueryIntFunction("int IsAggro()")') == "0"
    for actor in (4138, 4142, 4144):
        defeat(actor)
    time.sleep(2.5)
    assert _probe(host, 'ReadCharacterID(4143).QueryIntFunction("int IsAggro()")') == "0"
    assert story("progress") == 2
    # No KO message for the last deck guard. His settled character state
    # must start the intended boss sequence without another unrelated kill.
    code("knocked_out=_dead;", 4143)
    host.bridge.wait(lambda: story("progress") == 3, 35, what="deck defeat starts bodyguard")
    assert enabled(4155) and not enabled(4156) and not enabled(4150)
    _skip(host)
    defeat(4155)
    host.bridge.wait(lambda: story("progress") == 4, 35, what="bodyguard defeat starts cat boss")
    assert enabled(4156) and not enabled(4150)
    guest.bridge.wait(lambda: enabled(4156, guest) and not enabled(4150, guest), 25,
                      what="guest sees the boss and saved village encounter")
    for peer in (host, guest):
        assert not script_errors(peer), script_errors(peer)
    return {"passed": True, "metrics": {"checkpoint": 2, "bodyguard": 3, "cat_boss": 4}, "notes": []}


def well_sentry(ctx):
    """A silent KO behind the remaining White Flags guard does not reveal the player."""
    host = ctx.instance("well_sentry", keys={"mp_port": "28673", "difficulty_scaling": "false"})
    _start(host, "com-wolfire-overgrowth-campaign", "og_story/01_Slaver_Camp_Landing.xml")
    _skip(host)
    hero = host.bridge.me()["id"]
    # Use the actual well objective. Keep the player behind the guard, away
    # from the falling body, to isolate whether the objective discloses them.
    host.bridge.call("exec", target="level", code=f"""
        Hotspot@ h=cast<Hotspot@>(ReadObjectFromID(14));
        h.SetIntVar("progress",4); h.SetFloatVar("win_time",-1);
        MovementObject@ guard=ReadCharacterID(70);
        MovementObject@ player=ReadCharacterID({hero});
        player.position=Online_SafeSpotNear(guard.position,-guard.GetFacing(),24);
        player.Execute("FixDiscontinuity();");
    """)
    def notice():
        return _probe(host, 'ReadCharacterID(70).QueryIntFunction("int IsAggro()")')
    assert notice() == "0"
    host.bridge.call("exec", target=2207, code="SetKnockedOut(_unconscious);")
    time.sleep(2.5)  # More than two of the objective's one-second sentry checks.
    assert notice() == "0", "the well sentry acquired the player on the companion's KO"
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"remaining_guard": 70, "aggro": False}, "notes": []}
