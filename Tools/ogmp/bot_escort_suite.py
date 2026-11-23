"""Quiet roster escorts follow human stealth while ordinary NPC AI stays active.

Two isolated sandbox peers exercise real remote-owner crouch input, the
host-owned bot brain, player attack and enemy aggression cues, and disengaging.
"""

import time

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import _ready
from .scenarios import LEVEL, script_errors


def bot_escort_stealth(ctx):
    host = ctx.instance("escort_host", width=640, height=360,
                        keys={**RENDER_KEYS, "mp_port": "27994", "player_invincible": "true"})
    guest = ctx.instance("escort_guest", width=640, height=360,
                         keys={**RENDER_KEYS, "mp_port": "27995", "player_invincible": "true"})
    metrics = {}

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def rows(inst=host):
        return inst.bridge.status()["online"]["players"]

    def world(inst=host):
        return inst.bridge.state()

    def avatar(player, inst=host):
        row = next(p for p in rows(inst) if p["id"] == player)
        return next((c for c in world(inst) if c["id"] == row["avatar_id"]), None)

    def wait(predicate, label, seconds=25):
        return host.bridge.wait(predicate, seconds, step=0.1, what=label)

    def enemy_actor():
        return next(c for c in world() if c["id"] == enemy)

    def quiet_for(seconds):
        until = time.monotonic() + seconds
        samples = []
        while time.monotonic() < until:
            current = avatar(guest_bot)
            samples.append(current["ai_goal"])
            assert current["ai_goal"] != 1, ("quiet escort initiated combat", current)
            time.sleep(0.15)
        return samples

    def crouch(down):
        if down:
            guest.bridge.call("input", controller=0, hold={"crouch": 1.0}, steps=-1)
        else:
            guest.bridge.call("input", controller=0, release_all=True)

    def wait_stealth(label):
        wait(lambda: avatar(guest_bot)["ai_goal"] == 4 and
             avatar(guest_bot)["ai_escort_id"] == avatar(guest_human)["id"] and
             avatar(guest_bot).get("duck_amount", 0) > 0.6, label)

    code(host, 'LobbyBeginHost();LobbyAddBot();LobbyAddBot();'
         f'LobbySetSelection("sandbox","","Data/Levels/{LEVEL}",false);LobbySetForceTeams(true);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:27994")
    wait(lambda: len(rows()) == len(rows(guest)) == 4, "two humans and two escort bots")
    for player in rows():
        code(host, f'LobbySetPlayerTeam({player["id"]},1);', "menu")
    code(host, "LobbyStart();", "menu")
    _ready(host)
    _ready(guest)
    host_human = next(p["id"] for p in rows() if not p.get("bot") and p["peer_id"] == 255)
    guest_human = next(p["id"] for p in rows() if not p.get("bot") and p["peer_id"] != 255)
    bot_players = [p["id"] for p in rows() if p.get("bot")]
    # Authored enemies are irrelevant to this controlled perception fixture.
    roster_avatars = {p["avatar_id"] for p in rows()}
    excludes = "&&".join(f'c.GetID()!={ident}' for ident in roster_avatars)
    code(host, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
         f'if({excludes})ReadObjectFromID(c.GetID()).SetEnabled(false);' + '}')
    wait(lambda: all(avatar(p) and avatar(p)["ai_goal"] == 4 for p in bot_players), "both bots escort a human")
    wait(lambda: {avatar(p)["ai_escort_id"] for p in bot_players} ==
         {avatar(host_human)["id"], avatar(guest_human)["id"]}, "one bot follows each owner")
    guest_bot = next(p for p in bot_players if avatar(p)["ai_escort_id"] == avatar(guest_human)["id"])
    for p in (host_human, guest_human) + tuple(bot_players):
        inst = guest if p == guest_human else host
        code(inst, "invincible=true;", avatar(p, inst)["id"])
    base = avatar(guest_human)["pos"]
    enemy = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                             pos=[base[0] + 3, base[1] + 0.2, base[2] + 3], teams="escort_enemy")["object_id"]
    code(host, "always_unaware=true;listening=false;combat_allowed=false;chase_allowed=false;"
         "invincible=true;SetGoal(_patrol);", enemy)
    wait(lambda: enemy_actor().get("enabled", True), "ordinary NPC fixture is alive")
    assert not enemy_actor().get("lobby_escort_bot", False), "ordinary NPC gained a roster escort marker"

    # A perception notice alone must not turn an idle friendly escort into an
    # attacker. The hostile remains unaware of the human throughout this check.
    code(host, f'ReadCharacterID({avatar(guest_bot)["id"]}).ReceiveScriptMessage("notice {enemy}");')
    metrics["quiet_goals"] = quiet_for(2.5)
    crouch(True)
    wait(lambda: avatar(guest_human, guest).get("duck_amount", 0) > 0.6, "remote owner's real crouch input")
    wait_stealth("host-owned escort mirrors the remote owner's crouch")
    metrics["remote_crouch"] = {"human": avatar(guest_human, guest)["duck_amount"],
                                 "bot": avatar(guest_bot)["duck_amount"]}
    crouch(False)
    wait(lambda: avatar(guest_human, guest).get("duck_amount", 1) < 0.2 and
         avatar(guest_bot).get("duck_amount", 1) < 0.2, "releasing crouch stands both characters up")

    # Retry uses MovementObject::Reset/ResetGlobals, rather than replacing the
    # party. Native escort identity must survive the reset and keep the bot
    # quiet when it notices a peaceful target again on the fresh script state.
    code(host, f'ReadObjectFromID({enemy}).SetEnabled(false);')
    code(host, "permanent_health=0.5f;", avatar(guest_bot)["id"])
    code(host, "ResetLevel();")
    wait(lambda: avatar(guest_bot)["permanent_health"] > 0.99 and
         avatar(guest_bot).get("lobby_escort_bot") is True and
         avatar(guest_bot).get("lobby_escort_player_id") == avatar(guest_human)["id"] and
         avatar(guest_bot)["ai_goal"] == 4, "retry preserves quiet escort membership and buddy")
    for p in bot_players:
        code(host, "invincible=true;", avatar(p)["id"])
    code(host, "always_unaware=true;listening=false;combat_allowed=false;chase_allowed=false;"
         "invincible=true;SetGoal(_patrol);", enemy)
    code(host, f'ReadObjectFromID({enemy}).SetEnabled(true);'
         f'ReadCharacterID({avatar(guest_bot)["id"]}).ReceiveScriptMessage("notice {enemy}");')
    metrics["retry_quiet_goals"] = quiet_for(1.5)

    code(guest, "this_mo.static_char=true;SetTargetID(-1);SetState(_attack_state);",
         avatar(guest_human, guest)["id"])
    wait(lambda: avatar(guest_human)["state"] == 2 and avatar(guest_human)["ai_target_id"] == -1,
         "remote air swing reaches the host")
    metrics["air_swing_goals"] = quiet_for(1.0)
    code(guest, "SetState(_movement_state);this_mo.static_char=false;", avatar(guest_human, guest)["id"])

    # The player's own attack is an explicit request to engage, including when
    # its simulation authority is remote from the bot's host-owned brain.
    crouch(True)
    wait(lambda: avatar(guest_human, guest).get("duck_amount", 0) > 0.6, "buddy crouches before combat")
    enemy_common = enemy_actor()["common_id"]
    remote_enemy = next(c for c in world(guest) if c["common_id"] == enemy_common)
    code(guest, f'this_mo.static_char=true;SetTargetID({remote_enemy["id"]});SetState(_attack_state);',
         avatar(guest_human, guest)["id"])
    wait(lambda: avatar(guest_bot)["ai_goal"] == 1, "escort engages after its buddy starts a fight")
    metrics["player_attack"] = {"goal": avatar(guest_bot)["ai_goal"], "enemy": enemy}
    code(guest, "SetTargetID(-1);SetState(_movement_state);attacked_by_id=-1;this_mo.static_char=false;",
         avatar(guest_human, guest)["id"])
    code(host, f'ReadObjectFromID({enemy}).SetEnabled(false);')
    wait_stealth("escort resumes crouching with its buddy after combat ends")

    # A normal NPC may still initiate combat. Its actual aggression toward the
    # buddy should draw the escort into defense rather than suppressing NPC AI.
    code(host, f'ReadObjectFromID({enemy}).SetEnabled(true);')
    code(host, "always_unaware=false;combat_allowed=true;chase_allowed=true;SetGoal(_patrol);", enemy)
    code(host, f'ReadCharacterID({enemy}).ReceiveScriptMessage("notice {avatar(guest_human)["id"]}");')
    wait(lambda: enemy_actor()["ai_goal"] == 1, "ordinary NPC retains autonomous aggression")
    wait(lambda: avatar(guest_bot)["ai_goal"] == 1, "escort defends its buddy from an attacking NPC")
    metrics["defense"] = {"npc_goal": enemy_actor()["ai_goal"], "bot_goal": avatar(guest_bot)["ai_goal"]}
    code(host, f'ReadObjectFromID({enemy}).SetEnabled(false);')
    code(guest, "SetTargetID(-1);SetState(_movement_state);attacked_by_id=-1;", avatar(guest_human, guest)["id"])
    wait_stealth("escort returns to stealth after defending its buddy")
    quiet_for(1.5)
    crouch(False)
    assert avatar(guest_bot)["proxy"] is False
    assert avatar(guest_bot, guest)["proxy"] is True
    assert avatar(guest_human)["proxy"] is True and avatar(guest_human, guest)["proxy"] is False
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:8])
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
    return {"passed": True, "metrics": metrics, "notes": []}
