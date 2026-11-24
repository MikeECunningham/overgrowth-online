"""First campaign tutorial: bot joins during intro, paused creation and escorts.

Checks object geometry as well as character poses; decal failures can precede
non-finite character state. Uses only the isolated process-local input bridge.
"""

import re
import time
import math

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, FIRST_ID, _ready, _skip
from .scenarios import LEVEL as SANDBOX_LEVEL, script_errors


def _inject_decal_guard(host):
    def code(text):
        return host.bridge.call("exec", target="level", code=text)
    def wait(predicate, label):
        return host.bridge.wait(predicate, 15, step=0.1, what=label)
    code('int id=CreateObject("Data/Objects/Decals/blob_shadow.xml",true);'
         'float bad=sqrt(-1.0f);ReadObjectFromID(id).SetTranslation(vec3(bad,0,0));')
    detected = wait(lambda: host.bridge.call("nan_scan")["objects"], "scanner detects malformed decal")
    assert len(detected) == 1 and detected[0]["path"] == "Data/Objects/Decals/blob_shadow.xml", detected
    invalid_id = detected[0]["id"]
    wait(lambda: any("Skipping invalid clustered" in line and "world transform" in line and
                     f"source={invalid_id};" in line for line in host.log().splitlines()),
         "renderer diagnoses and skips the malformed decal")
    injected_scan = host.bridge.call("nan_scan")
    assert not injected_scan.get("characters") and not injected_scan.get("cameras"), injected_scan
    assert {c["id"] for c in injected_scan["objects"]} == {invalid_id}, injected_scan
    code(f'DeleteObjectID({invalid_id});')
    wait(lambda: not host.bridge.call("nan_scan")["objects"], "remove malformed decal fixture")
    cleaned = host.bridge.call("nan_scan")
    assert not any(cleaned.get(key) for key in ("objects", "characters", "cameras")), cleaned
    assert not re.search(r'proj_point\.w|Assertion failed', host.log(), re.I)
    assert not script_errors(host), script_errors(host)[:8]
    return {"object": invalid_id, "injected_scan": injected_scan, "cleaned_scan": cleaned}


def tutorial_decal_guard(ctx):
    host = ctx.instance("tutorial_decal_guard", level=SANDBOX_LEVEL, width=640, height=360,
                        keys={**RENDER_KEYS, "no_reflection_capture": "false", "player_invincible": "true"})
    _ready(host)
    clean = host.bridge.call("nan_scan")
    assert not any(clean.get(key) for key in ("objects", "characters", "cameras")), clean
    return {"passed": True, "metrics": _inject_decal_guard(host), "notes": []}


def tutorial_bot_overlap(ctx):
    host = ctx.instance("tutorial_bot_overlap", level=SANDBOX_LEVEL, width=640, height=360,
                        keys={**RENDER_KEYS, "player_invincible": "true"})
    _ready(host)
    def code(text, target="level"):
        return host.bridge.call("exec", target=target, code=text)
    def wait(predicate, label):
        return host.bridge.wait(predicate, 25, step=0.1, what=label)
    code('LobbyAddBot();LobbyAddBot();')
    wait(lambda: len(host.bridge.status()["online"]["players"]) == 3, "two local collision fixtures")
    def bots():
        ids = {p["avatar_id"] for p in host.bridge.status()["online"]["players"] if p.get("bot")}
        return sorted((c for c in host.bridge.state() if c["id"] in ids), key=lambda c: c["id"])
    wait(lambda: len(bots()) == 2 and all(c.get("updated", 0) > 3 for c in bots()), "healthy initialized bot poses")
    code('LobbySetPaused(true);')
    wait(lambda: host.bridge.status()["paused"], "pause for an atomic collision test")
    pair = [c["id"] for c in bots()]
    results = []
    for mine, other in (pair, list(reversed(pair))):
        host.bridge.call("level_messages", prefix="bot_overlap")
        setup = 'Recover();coop_cutscene_hold=false;reset_no_collide=-100.0f;this_mo.static_char=false;'
        setup += 'SetState(_movement_state);push_velocity=vec3(0);time_step=1.0f/120.0f;'
        code(setup, other)
        code(setup + f'MovementObject@ other=ReadCharacterID({other});other.position=this_mo.position;'
             'Skeleton@ a=this_mo.rigged_object().skeleton();Skeleton@ b=other.rigged_object().skeleton();'
             # Zero physical origins guarantee exact COM equality even if
             # templates have different mass distributions. Simulation stays
             # paused throughout this deliberately degenerate fixture.
             'for(int i=0;i<a.NumBones();++i)if(a.HasPhysics(i)&&b.HasPhysics(i)){'
             'mat4 t=a.GetBoneTransform(i);t.SetTranslationPart(vec3(0));'
             'a.SetBoneTransform(i,t);b.SetBoneTransform(i,t);}'
             'vec3 ac=a.GetCenterOfMass();vec3 bc=b.GetCenterOfMass();ac.y=this_mo.position.y;bc.y=other.position.y;'
             'level.SendMessage("bot_overlap_before "+distance_squared(ac,bc));'
             'HandleCollisionsBetweenTwoCharacters(other);'
             'level.SendMessage("bot_overlap_after "+distance(this_mo.position,other.position));'
             'level.SendMessage("bot_overlap_push "+length_squared(push_velocity));', mine)
        code('level.SendMessage("bot_overlap_other_push "+length_squared(push_velocity));', other)
        messages = host.bridge.call("level_messages", prefix="bot_overlap")["messages"]
        values = dict((line.split()[0], float(line.split()[1])) for line in messages)
        assert all(k in values for k in ("bot_overlap_before", "bot_overlap_after", "bot_overlap_push", "bot_overlap_other_push")), messages
        assert values["bot_overlap_before"] <= 1e-12, ("fixture centers were not identical", values)
        assert math.isclose(values["bot_overlap_after"], 0.7, abs_tol=1e-4), values
        assert all(math.isfinite(n) for n in values.values()), values
        scan = host.bridge.call("nan_scan")
        assert not any(scan.get(k) for k in ("objects", "characters", "cameras")), scan
        current = bots()
        assert current[0]["pos"][0] < current[1]["pos"][0], "collision direction changed with callback order"
        results.append(values)
    assert not script_errors(host), script_errors(host)[:8]
    return {"passed": True, "metrics": {"pair": pair, "directions": results}, "notes": []}


def tutorial_bots_nan(ctx, local_only=False, reflections=True, inject_bad_decal=False):
    name = "tutorial_bots_nan_local" if local_only else "tutorial_bots_nan"
    host = ctx.instance(name, width=640, height=360,
                        keys={**RENDER_KEYS, "no_reflection_capture": "false" if reflections else "true",
                              "mp_port": "28034", "player_invincible": "true",
                              "test_start_paused": "false", "campaign_mp_scaling": "true"})
    metrics = {"local_only": local_only, "reflections": reflections, "phases": []}
    ctx.tutorial_bot_metrics = metrics

    def code(text, target="level"):
        return host.bridge.call("exec", target=target, code=text)

    def status():
        return host.bridge.status()

    def wait(predicate, label):
        return host.bridge.wait(predicate, 35, step=0.1, what=label)

    def check(phase):
        scan = host.bridge.call("nan_scan")
        bad = {key: scan[key] for key in ("objects", "characters", "cameras", "decals", "invalid_decals", "invalid_decal_geometry") if scan.get(key)}
        assert not bad, (phase, bad)
        lines = [line for line in host.log().splitlines()
                 if re.search(r'proj_point\.w|\bNaN\b|\bInfinity\b|Assertion failed', line, re.I)]
        assert not lines, (phase, lines[-12:])
        return scan

    def sample(phase, seconds):
        start = time.monotonic()
        while time.monotonic() - start < seconds:
            check(phase)
            time.sleep(0.1)
        s = status()
        avatars = {p["avatar_id"] for p in s["online"]["players"]}
        actors = [{k: c.get(k) for k in ("id", "common_id", "pos", "visible", "enabled", "proxy", "updated",
                                        "shadow_id", "lf_shadow_id", "rf_shadow_id", "state",
                                        "coop_cutscene_hold", "ai_goal", "ai_escort_id")}
                  for c in host.bridge.state() if c["id"] in avatars]
        host.bridge.call("level_messages", prefix="bot_pose_probe")
        code('for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(Online_IsPlayerCharacter(c.GetID()))level.SendMessage("bot_pose_probe "+c.GetID()+"|"+'
             'c.GetIntVar("updated")+"|"+c.GetBoolVar("queue_fix_discontinuity")+"|"+'
             'c.GetFloatVar("time")+"|"+c.rigged_object().anim_client().GetCurrAnim());}')
        animation = host.bridge.call("level_messages", prefix="bot_pose_probe")["messages"]
        metrics["phases"].append({"phase": phase, "dialogue": s["dialogue_camera_control"],
                                   "cutscene": s["coop_cutscene"], "paused": s["paused"], "actors": actors,
                                   "animation": animation})

    if local_only:
        host.bridge.call("load_level", path="set_campaign " + CAMPAIGN)
        host.bridge.call("load_level", path="load_campaign_level " + FIRST_ID)
    else:
        code('LobbyBeginHost();for(int i=0;i<4;++i)LobbyAddBot();'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();', "menu")
    _ready(host, FIRST)
    if local_only:
        code('for(int i=0;i<4;++i)LobbyAddBot();')
        wait(lambda: len(status()["online"]["players"]) == 5, "first four bots create a private campaign party")
    sample("four bots during first tutorial intro", 3)
    code('LobbyAddBot();LobbyAddBot();')
    wait(lambda: len(status()["online"]["players"]) == 7, "two bots join during intro")
    sample("six bots after intro joins", 3)
    _skip(host)
    sample("six bots after skipping dialogue", 4)

    host.bridge.call("input", controller=0, hold={"quit": 1}, steps=1)
    wait(lambda: 0 in status()["open_menu_players"], "host's personal pause menu")
    manual_pause = not status()["paused"]
    if manual_pause:
        host.bridge.call("exec", target="player_menu", local_player=0,
                         code='imGUI.receiveMessage(IMMessage("toggle_world_pause"));')
    wait(lambda: status()["paused"], "host pauses the shared world")
    for _ in range(2):
        host.bridge.call("exec", target="player_menu", local_player=0,
                         code='imGUI.receiveMessage(IMMessage("add_lobby_bot"));')
        sample("paused bot creation", 1)
    wait(lambda: len(status()["online"]["players"]) == 9, "eight friendly bots")
    if manual_pause:
        host.bridge.call("exec", target="player_menu", local_player=0,
                         code='imGUI.receiveMessage(IMMessage("toggle_world_pause"));')
    host.bridge.call("exec", target="player_menu", local_player=0,
                     code='imGUI.receiveMessage(IMMessage("Continue"));')
    wait(lambda: not status()["paused"] and not status()["open_menu_players"], "resume after bot additions")
    sample("eight bots after resume", 4)

    host.bridge.call("input", controller=0, hold={"up": 1}, steps=240)
    sample("eight bots escort moving main character", 3)
    host.bridge.call("input", controller=0, release_all=True)
    sample("eight bots idle after escort", 3)
    _skip(host)  # Walking may have entered another authored conversation.
    hero = host.bridge.me()
    enemies = []
    for index in range(3):
        pos = [hero["pos"][0] + 2.5 + index * 0.6, hero["pos"][1] + 0.2, hero["pos"][2] + 2]
        enemies.append(host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                                        pos=pos, teams="tutorial_nan_enemy")["object_id"])
    code(f'SetTargetID({enemies[0]});', hero["id"])
    host.bridge.call("input", controller=0, hold={"attack": 1}, steps=720)
    sample("eight bots join a real melee encounter", 7)
    host.bridge.call("input", controller=0, release_all=True)
    for enemy in enemies:
        code(f'ReadObjectFromID({enemy}).SetEnabled(false);')
    code('level.SendMessage("reset");')
    sample("eight bots after tutorial retry", 3)
    _skip(host)
    sample("eight bots after retry dialogue", 3)
    assert not script_errors(host), script_errors(host)[:8]
    metrics["final_scan"] = check("final")
    metrics["natural_invalid_cluster_warnings"] = [line for line in host.log().splitlines()
                                                     if "Skipping invalid clustered" in line]
    if inject_bad_decal:
        # Fault injection is separate from all natural finite-state checks.
        # A malformed decal must be diagnosed and skipped without poisoning
        # characters/cameras or raising the former projection assertion.
        metrics["guard_injection"] = _inject_decal_guard(host)
    return {"passed": True, "metrics": metrics, "notes": []}
