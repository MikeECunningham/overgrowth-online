"""splitscreen_join: a player with split screen on, and another player joins.

A tester's PC crashed the moment another player joined while split screen
was on. Split screen draws one viewport per player character, and online the
other players' characters count too. Host and guest both have split_screen
on; the guest joins. Both must keep running, with no GL problems and no NaN.

    python -I Tools/ogmp.py run splitscreen_join
"""

import time

from .scenarios import PROFILES, LEVEL, dist, join, wait_loaded


def local_splitscreen(ctx):
    """The pause button adds nine local views, with independent P5 input.

    A full level reload must retain the shared party and its screen assignments.
    Run with OGMP_KEYS=renderer=vulkan to exercise that backend too.
    """
    inst = ctx.instance("local_splitscreen", level=LEVEL, width=1280, height=720,
                        keys={"split_screen": "false", "gl_debug": "1"})
    wait_loaded(inst)
    bridge = inst.bridge
    before = bridge.state()
    npc_ids = {c["id"] for c in before if not c["is_player"]}
    m, fails = {"counts": []}, []

    def execute(code):
        bridge.call("exec", target="level", code=code)

    bridge.call("input", controller=0, hold={"quit": 1.0}, steps=1)
    bridge.wait(lambda: bridge.status()["open_menu_players"] == [0] and bridge.status()["paused"],
                10, what="single-player pause menu")
    for count in range(2, 10):
        bridge.call("exec", target="player_menu", local_player=0,
                    code='imGUI.receiveMessage(IMMessage("debug_add_local_player"));')
        bridge.wait(lambda: bridge.status()["local_players"] == count, 20,
                    what=f"debug local player {count}")
        bridge.wait(lambda: bridge.status()["local_inputs"] == count, 10,
                    what=f"local input slots for {count} players")
        players = [c for c in bridge.state() if c["is_player"]]
        m["counts"].append(len(players))
        if {c["controller_id"] for c in players} != set(range(count)):
            fails.append(f"{count} players: controllers are not unique contiguous slots")
        if {c["camera_id"] for c in players} != set(range(count)):
            fails.append(f"{count} players: cameras are not unique contiguous slots")
    ctx.shot(inst, "pause_nine")

    bridge.call("exec", target="player_menu", local_player=0,
                code='this_ui.SendCallback("close");')
    bridge.wait(lambda: not bridge.status()["open_menu_players"] and not bridge.status()["paused"],
                10, what="continue local play")
    time.sleep(0.5)
    start = {c["controller_id"]: c for c in bridge.state() if c["is_player"]}
    bridge.call("input", controller=4, hold={"up": 1.0}, steps=180)
    time.sleep(1.5)
    bridge.call("input", controller=4, release_all=True)
    end = {c["controller_id"]: c for c in bridge.state() if c["is_player"]}
    m["p5_moved_m"] = round(dist(start[4]["pos"], end[4]["pos"]), 3)
    m["p1_moved_m"] = round(dist(start[0]["pos"], end[0]["pos"]), 3)
    if m["p5_moved_m"] < 1.0 or m["p1_moved_m"] > 0.5:
        fails.append("P5 input did not move P5 independently of P1")
    if {c["id"] for c in bridge.state() if not c["is_player"]} != npc_ids:
        fails.append("debug players changed the NPC roster")
    ctx.shot(inst, "nine_screens")
    health = bridge.call("nan_scan")
    m["nan_characters"] = len(health.get("characters", []))
    if m["nan_characters"]:
        fails.append("non-finite character state")
    status = bridge.status()
    m["renderer"] = status["renderer"]
    m["render_problems"] = status.get("vk_problems", 0) + status["gl_problems"]
    if m["render_problems"]:
        fails.append("renderer reported problems with nine screens")

    roster_before = {p["id"]: (p["name"], p["team"], p["screen"], p["local_index"])
                     for p in status["online"]["players"]}
    bridge.call("load_level", path="Data/Levels/" + LEVEL, discard=True)
    bridge.wait(lambda: bridge.status()["level_loaded"] and bridge.status()["local_players"] == 9,
                150, what="persistent nine-player party after reload")
    status = bridge.status()
    m["after_reload"] = {k: status[k] for k in ("local_players", "local_inputs", "split_screen")}
    roster_after = {p["id"]: (p["name"], p["team"], p["screen"], p["local_index"])
                    for p in status["online"]["players"]}
    if status["local_inputs"] != 9 or not status["split_screen"] or roster_before != roster_after:
        fails.append("shared party identities, controls or screens changed during reload")
    # Adding another player after reload uses the same persistent roster.
    bridge.call("input", controller=0, hold={"quit": 1.0}, steps=1)
    bridge.wait(lambda: bridge.status()["open_menu_players"] == [0], 10, what="party pause menu after reload")
    bridge.call("exec", target="player_menu", local_player=0,
                code='imGUI.receiveMessage(IMMessage("debug_add_local_player"));')
    bridge.wait(lambda: bridge.status()["local_players"] == 10, 20,
                what="tenth local player after reload")
    return {"passed": not fails, "metrics": m, "notes": fails}


def splitscreen_join(ctx, profile="lan"):
    m, fails = {}, []
    keys = {"split_screen": "true"}
    host = ctx.instance("host", keys=keys)
    host.bridge.call("net_sim", **PROFILES[profile])
    host.bridge.call("host", level=LEVEL)
    wait_loaded(host)
    host.bridge.wait(lambda: host.bridge.call("start_session")["started"], 60, what="session started")
    time.sleep(2.0)
    m["host_gl_before"] = host.bridge.call("gl_problems")["count"]
    ctx.shot(host, "alone")

    g = ctx.instance("client1", keys=keys)
    g.bridge.call("net_sim", **PROFILES[profile])
    try:
        join(ctx, host, g)
    except Exception as e:
        fails.append(f"join: {e}")
    for _ in range(10):
        time.sleep(1.0)
        if not (host.alive() and g.alive()):
            break

    m["host_alive"], m["guest_alive"] = host.alive(), g.alive()
    for inst, name in ((host, "host"), (g, "guest")):
        if not inst.alive():
            fails.append(f"the {name} died")
            continue
        gl = inst.bridge.call("gl_problems")
        m[f"{name}_gl"] = gl["count"]
        m[f"{name}_gl_list"] = gl.get("problems", [])[:5]
        nan = inst.bridge.call("nan_scan")
        m[f"{name}_nan_chars"] = len(nan.get("characters", []))
        ctx.shot(inst, "joined")
        if gl["count"] > m.get("host_gl_before", 0) and name == "host":
            fails.append("the host's GL debug output reports problems after the join")
        if name == "guest" and gl["count"] > 0:
            fails.append("the guest's GL debug output reports problems")
        if m[f"{name}_nan_chars"]:
            fails.append(f"NaN characters on the {name}")
    return {"passed": not fails, "metrics": m, "notes": fails}
