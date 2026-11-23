"""Secondary local input and reliable controls under a crowded state stream."""

import math
import time

from .newcampaign_local_suite import _ready
from .scenarios import script_errors


def crowded_actions(ctx):
    host = ctx.instance("crowd_host", keys={"mp_port": "27964"})
    guest = ctx.instance("crowd_guest", keys={"mp_port": "27965"})
    machines = (host, guest)

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def online(inst):
        return inst.bridge.status()["online"]

    def wait(predicate, name, timeout=40):
        return host.bridge.wait(predicate, timeout, step=0.1, what=name)

    def actor(inst, player_id):
        row = next(p for p in online(inst)["players"] if p["id"] == player_id)
        return next(c for c in inst.bridge.state() if c["id"] == row["avatar_id"])

    def vector(values):
        return "vec3(" + ",".join(str(float(v)) for v in values) + ")"

    code(host, 'LobbyBeginHost(); LobbyAddLocalPlayer(); LobbySetAllowAllPause(true); '
               'for(int i=0;i<20;++i) LobbyAddBot(); '
               'LobbySetSelection("sandbox","","Data/Levels/Project60/4_red_hills.xml",false);', "menu")
    code(guest, 'LobbyAddLocalPlayer();', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:27964")
    wait(lambda: len(online(host)["players"]) == 24 and len(online(guest)["players"]) == 24, "crowded pregame roster")
    code(host, "LobbyStart();", "menu")
    for inst in machines: _ready(inst)
    wait(lambda: all(len(inst.bridge.state()) == 24 for inst in machines), "all 24 actors replicated")
    code(host, 'array<PlayerState>@ p=GetPlayerStates(); vec3 at=ReadCharacterID(p[0].avatar_id).position; '
               'for(int i=0;i<40;++i){int w=CreateObject("Data/Items/flint_knife.xml",true); '
               'ReadObjectFromID(w).SetTranslation(at+vec3(12.0f+float(i%8),0.4f,12.0f+float(i/8)));}')
    wait(lambda: all(len(inst.bridge.call("state")["items"]) == 40 for inst in machines), "loose-item world replicated")
    for inst in machines:
        inst.bridge.call("net_sim", latency_ms=70, jitter_ms=30, loss=0.04)
    time.sleep(2.0)
    target = next(p["id"] for p in online(guest)["players"] if p["peer_id"] != 255 and p["local_index"] == 1)
    before = actor(guest, target)
    observed_before = actor(host, target)
    counters = {inst.name: online(inst) for inst in machines}
    guest.bridge.call("input", controller=before["controller_id"], hold={"right": 1.0, "jump": 1.0}, steps=180)
    time.sleep(3.0)
    guest.bridge.call("input", controller=before["controller_id"], release_all=True)
    time.sleep(1.0)
    own, seen = actor(guest, target), actor(host, target)
    owner_distance = math.dist(before["pos"], own["pos"])
    observer_distance = math.dist(observed_before["pos"], seen["pos"])
    movement_gap = math.dist(own["pos"], seen["pos"])
    assert owner_distance > 0.5 and observer_distance > 0.5, (owner_distance, observer_distance)
    assert movement_gap < 1.5, (own["pos"], seen["pos"])
    # The second local player's reliable global pause must pass the queued
    # character/item state traffic in both directions, then resume normally.
    started = time.monotonic()
    code(guest, 'SetConfigValueInt("controller_menu_owner",1); LobbySetPaused(true);')
    wait(lambda: all(inst.bridge.status()["paused"] for inst in machines), "secondary local pause under stream", 5)
    pause_seconds = time.monotonic() - started
    code(guest, 'LobbySetPaused(false);')
    wait(lambda: all(not inst.bridge.status()["paused"] for inst in machines), "secondary local resume", 5)

    # Exercise the real defender-authority request/reply path for the second
    # local unit, while the crowded state stream is still active.
    own = actor(guest, target)
    primary_id = next(p["id"] for p in online(guest)["players"] if p["peer_id"] != 255 and p["local_index"] == 0)
    primary = actor(guest, primary_id)
    if math.dist(own["pos"], primary["pos"]) < 7.0:
        guest.bridge.call("input", controller=own["controller_id"], hold={"right": 1.0, "jump": 1.0}, steps=180)
        time.sleep(3.0)
        guest.bridge.call("input", controller=own["controller_id"], release_all=True)
        time.sleep(0.6)
    own = actor(guest, target)
    victim_id = next(p["id"] for p in online(host)["players"] if p["bot"])
    victim = actor(host, victim_id)
    spot = [own["pos"][0] + 1.3, own["pos"][1], own["pos"][2]]
    code(host, f'Online_PlaceAvatar({victim["id"]},{vector(spot)},vec3(-1,0,0));')
    wait(lambda: math.dist(actor(guest, victim_id)["pos"], actor(guest, target)["pos"]) < 3.0,
         "nearby server bot beside secondary player")
    hit_before = {inst.name: online(inst) for inst in machines}
    replies = []
    for _ in range(3):
        attacker, defender = actor(guest, target), actor(guest, victim_id)
        previous = online(guest)["hit_results"]
        started = time.monotonic()
        code(guest, f'Online_SendHitRequest({defender["id"]},"blockprepare","Data/Attacks/frontkick.xml",'
                    f'normalize(ReadCharacterID({defender["id"]}).position-ReadCharacterID({attacker["id"]}).position),'
                    f'ReadCharacterID({defender["id"]}).position,{attacker["id"]},1.0f,1.0f,3.0f);')
        wait(lambda: online(guest)["hit_results"] > previous, "secondary request gets defender reply", 5)
        replies.append(time.monotonic() - started)
    assert online(host)["hits_resolved"] >= hit_before[host.name]["hits_resolved"] + 3
    assert online(guest)["hits_requested"] >= hit_before[guest.name]["hits_requested"] + 3

    # Temporary control must use any nearby owned local unit for admission,
    # rather than only this client's primary seat. The server's bot returns
    # to its usual owner after the explicit release.
    defender = actor(guest, victim_id)
    primary_gap = math.dist(actor(guest, primary_id)["pos"], defender["pos"])
    assert primary_gap > 5.0, ("primary too close to prove secondary ownership", primary_gap)
    takeovers = online(guest)["takeovers"]
    code(guest, f'Online_TakeOver({defender["id"]},4.0f);')
    wait(lambda: online(host)["overrides"] > 0 and online(guest)["takeovers"] > takeovers and
         not actor(guest, victim_id)["proxy"] and actor(host, victim_id)["proxy"],
         "secondary nearby takeover admitted by host", 5)
    code(guest, f'Online_ReleaseTakeOver({defender["id"]});')
    wait(lambda: online(host)["overrides"] == 0 and online(guest)["overrides"] == 0 and
         actor(guest, victim_id)["proxy"] and not actor(host, victim_id)["proxy"], "bot ownership handed back", 5)
    final = {inst.name: online(inst) for inst in machines}
    assert final[host.name]["item_updates_idle_skipped"] > counters[host.name]["item_updates_idle_skipped"]
    for inst in machines:
        status = inst.bridge.status()
        assert status["local_inputs"] == 2 and status["render_views"] == 2, status
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
        assert not script_errors(inst), script_errors(inst)[:5]
    return {"passed": True, "metrics": {"actors": 24, "loose_items": 40,
        "owner_moved_m": owner_distance, "observer_moved_m": observer_distance,
        "final_position_gap_m": movement_gap, "pause_seconds": pause_seconds,
        "secondary_hit_reply_seconds": replies, "secondary_takeover_primary_distance_m": primary_gap,
        "secondary_takeovers": online(guest)["takeovers"] - takeovers,
        "counters_before": counters, "counters_after": final}, "notes": []}
