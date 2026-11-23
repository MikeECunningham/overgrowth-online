"""Regression checks for preview-camera flares, arena feet-level markers,
and loose weapon playback outside the host's camera.
"""

import math
import time

from .scenarios import script_errors

CAVE = "Data/Levels/arenas/Cave_Arena.xml"
STUCCO = "arenas/stucco_courtyard_arena.xml"


def _loaded(inst):
    inst.bridge.wait(lambda: (lambda s: s["level_loaded"] and not s["waiting_for_input"])(
        inst.bridge.call("status", timeout=240)), 240, what="level and first frame")


def _menu(inst, code):
    return inst.bridge.call("exec", target="menu", code=code)


def _arena(inst, code):
    return inst.bridge.call("exec", target="level", context="level_specific", code=code)


def stucco_solo(ctx, renderer="vulkan"):
    """Light-probe capture must survive its negative-ID preview camera."""
    inst = ctx.instance("stucco-solo", level=STUCCO, keys={"renderer": renderer})
    _loaded(inst)
    time.sleep(4)
    state = inst.bridge.status()
    chars = inst.bridge.state()
    ctx.shot(inst, "stucco_solo")
    errors = script_errors(inst)
    return {"passed": inst.alive() and bool(chars) and not errors,
            "metrics": {"renderer": state.get("renderer"), "characters": len(chars),
                        "nan": inst.bridge.call("nan_scan")},
            "notes": [str(e) for e in errors[:4]]}


def cave_spawns(ctx, rounds=4):
    """Two players on each peer, authored arena spawns and character switches."""
    keys = {"oo_arena_bots": "0", "oo_arena_random": "0"}
    host = ctx.instance("cave-spawn-host", keys=keys)
    guest = ctx.instance("cave-spawn-guest", keys=keys)
    _menu(host, 'LobbyBeginHost(); LobbyAddLocalPlayer(); '
          'LobbySetSelection("com-wolfire-arena","com-wolfire-arena","' + CAVE + '",false);')
    _menu(guest, "LobbyAddLocalPlayer();")
    port = host.keys.get("mp_port")
    guest.bridge.call("connect", ip="127.0.0.1" + (":" + str(port) if port else ""))
    host.bridge.wait(lambda: len(host.bridge.status()["online"]["players"]) == 4,
                     30, what="four lobby seats")
    _menu(host, "LobbyStart();")
    for inst in (host, guest):
        _loaded(inst)
    metrics, faults = {"rounds": []}, []
    for round_index in range(rounds):
        if round_index:
            _arena(host, 'SetConfigValueBool("oo_arena_random", ' +
                   ("true" if round_index % 2 else "false") + '); NewMatch();')
        time.sleep(5)
        result = {}
        host.bridge.call("level_messages", prefix="cave_target_probe")
        _arena(host, '''for(uint k=0;k<roster.length();++k) {
            vec3 p,f; SpotFor(k,p,f);
            col.GetObjRayCollision(p+vec3(0,2,0),p-vec3(0,2,0));
            float ground=-1000000.0f;
            for(int j=0;j<sphere_col.NumContacts();++j) {
                CollisionPoint hit=sphere_col.GetContact(j);
                ground=max(ground,hit.position.y);
            }
            level.SendMessage("cave_target_probe "+k+" "+p.y+" "+ground);
        }''')
        targets = host.bridge.call("level_messages", prefix="cave_target_probe")["messages"]
        result["placement_targets"] = targets
        if len(targets) != 4:
            faults.append("missing arena placement targets")
        for line in targets:
            _, slot, y, ground = line.split()
            if not (0.3 <= float(y) - float(ground) < 1.7):
                faults.append(f"slot {slot}: placement target embedded in the floor")
        for inst in (host, guest):
            inst.bridge.call("level_messages", prefix="cave_spawn_probe")
            # Sample the upper, walkable rock surface, not the lower terrain
            # hidden underneath the authored Rock Cave floor.
            _arena(inst, '''for(int k=0;k<GetNumCharacters();++k) {
                MovementObject@ c=ReadCharacter(k); vec3 p=c.position;
                col.GetObjRayCollision(p+vec3(0,4,0),p-vec3(0,4,0));
                float ground=-1000000.0f;
                for(int j=0;j<sphere_col.NumContacts();++j) {
                    CollisionPoint hit=sphere_col.GetContact(j);
                    ground=max(ground,hit.position.y);
                }
                level.SendMessage("cave_spawn_probe "+c.GetID()+" "+p.y+" "+ground);
            }''')
            lines = inst.bridge.call("level_messages", prefix="cave_spawn_probe")["messages"]
            poses = inst.bridge.state()
            result[inst.name] = {"poses": poses, "ground_probes": lines}
            if len(poses) != 4 or len(lines) != 4:
                faults.append(f"{inst.name}: expected four fighters and four ground probes")
            for line in lines:
                _, actor, y, ground = line.split()
                clearance = float(y) - float(ground)
                if not (0.2 < clearance < 1.7):
                    faults.append(f"{inst.name}: actor {actor} floor clearance {clearance:.3f} m")
            if any(c["knocked_out"] for c in poses):
                faults.append(f"{inst.name}: fighter down immediately after placement")
            if inst.bridge.call("nan_scan")["characters"]:
                faults.append(f"{inst.name}: nonfinite character")
        metrics["rounds"].append(result)
    for inst in (host, guest):
        ctx.shot(inst, "cave_spawn_" + inst.name)
        faults.extend(str(e) for e in script_errors(inst)[:4])
    return {"passed": not faults, "metrics": metrics, "notes": faults}


def loose_weapon_playback(ctx):
    """A flying knife still streams when every host camera looks elsewhere."""
    host = ctx.instance("weapon-playback-host")
    guest = ctx.instance("weapon-playback-guest")
    _menu(host, 'LobbyBeginHost(); LobbySetSelection("sandbox","",'
          '"Data/Levels/Project60/4_red_hills.xml",false);')
    port = host.keys.get("mp_port")
    guest.bridge.call("connect", ip="127.0.0.1" + (":" + str(port) if port else ""))
    host.bridge.wait(lambda: len(host.bridge.status()["online"]["players"]) == 2,
                     30, what="two lobby seats")
    _menu(host, "LobbyStart();")
    for inst in (host, guest):
        _loaded(inst)
    time.sleep(1)
    p = guest.bridge.me()["pos"]
    launch = [p[0], p[1] + 4, p[2] + 4]
    host.bridge.call("view", pos=[1000, 1000, 1000], target=[1100, 1000, 1000], pause=False)
    item = host.bridge.call("spawn", path="Data/Items/flint_knife.xml", pos=launch)["object_id"]
    host.bridge.call("exec", target="level", code=f"ReadItemID({item}).ActivatePhysics(); "
                     f"ReadItemID({item}).SetLinearVelocity(vec3(12,0,0)); ReadItemID({item}).SetThrown();")
    samples = []
    t0 = time.monotonic()
    while time.monotonic() - t0 < 0.65:
        h = next((i for i in host.bridge.call("state")["items"] if i["id"] == item), None)
        g = next((i for i in guest.bridge.call("state")["items"] if i["common_id"] == item), None)
        if h and g:
            samples.append({"t": time.monotonic() - t0, "host": h["pos"], "guest": g["draw_pos"]})
        time.sleep(0.015)
    faults = []
    if len(samples) < 5:
        faults.append("too few playback samples")
    else:
        host_motion = samples[-1]["host"][0] - samples[0]["host"][0]
        guest_motion = samples[-1]["guest"][0] - samples[0]["guest"][0]
        if host_motion < 3 or guest_motion < 2.5:
            faults.append(f"weapon did not fly smoothly offscreen: host {host_motion:.2f}, guest {guest_motion:.2f} m")
        for a, b in zip(samples, samples[1:]):
            gap = b["t"] - a["t"]
            if math.dist(a["guest"], b["guest"]) > 18 * gap + 0.4:
                faults.append("remote flying weapon jumped between snapshots")
                break
    host.bridge.call("view", clear=True)
    return {"passed": not faults, "metrics": {"samples": samples}, "notes": faults}
