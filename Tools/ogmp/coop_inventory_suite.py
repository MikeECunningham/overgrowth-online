"""Physical campaign weapons survive retry, replication, drops and late joins."""

import time

from .newcampaign_local_suite import _ready, _skip
from .scenarios import script_errors


def coop_inventory(ctx):
    host = ctx.instance("inventory_host", keys={"mp_port": "27944"})
    guest = ctx.instance("inventory_guest", keys={"mp_port": "27945"})
    machines = [host, guest]
    metrics = {"rounds": []}

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def rows(inst):
        return inst.bridge.status()["online"]["players"]

    def state(inst):
        return inst.bridge.call("state")

    def human(inst, player):
        avatar = next(p["avatar_id"] for p in rows(inst) if p["id"] == player)
        return next(c for c in state(inst)["characters"] if c["id"] == avatar)

    def wait(predicate, label, timeout=30):
        return host.bridge.wait(predicate, timeout, step=0.1, what=label)

    def inventories():
        result = []
        for inst in machines:
            snapshot = state(inst)
            actors = {c["id"]: c for c in snapshot["characters"]}
            inventory = {}
            for player in rows(inst):
                actor = actors.get(player["avatar_id"])
                if actor is None: return None
                slots = [v for v in actor["weapon_slots_common"] if v >= 0]
                attached = [v["common_id"] for v in actor["attachments"]]
                held = [v["common_id"] for v in snapshot["items"] if v["held_by"] == actor["id"]]
                if len(set(slots)) != len(slots) or sorted(slots) != sorted(attached) or sorted(held) != sorted(attached):
                    return None
                inventory[player["id"]] = {"slots": actor["weapon_slots_common"],
                    "attachments": sorted([{k: v[k] for k in ("common_id", "state", "bone_id")}
                        for v in actor["attachments"]], key=lambda item: item["common_id"])}
            result.append(inventory)
        if not all(value == result[0] for value in result[1:]): return None
        return result[0]

    def consistent(label, count=None):
        def ready():
            if count is not None and any(len(state(inst)["items"]) != count for inst in machines): return None
            return inventories()
        return wait(ready, label)

    # Real authored campaign equipment: one DogSword on the hero's right hip.
    code(host, 'LobbyBeginHost(); LobbySetSelection("com-wolfire-overgrowth-campaign", '
               '"com-wolfire-overgrowth-campaign", "Data/Levels/og_story/08_Ice_Cliff_Landing.xml", false);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:27944")
    wait(lambda: len(rows(host)) == 2 and len(rows(guest)) == 2, "pregame equipment roster")
    code(host, "LobbyStart();", "menu")
    for inst in machines: _ready(inst)
    _skip(host)
    wait(lambda: all(len([it for it in state(inst)["items"] if it["held_by"] >= 0]) == 2 for inst in machines),
         "guest receives campaign sword", 40)
    initial = consistent("initial physical swords", 2)
    assert all(len(v["attachments"]) == 1 for v in initial.values()), initial
    identities = {p: v["attachments"][0]["common_id"] for p, v in initial.items()}
    guest_id = next(p["id"] for p in rows(host) if p["peer_id"] != 255)
    host_id = next(p["id"] for p in rows(host) if p["peer_id"] == 255)
    metrics["initial"] = initial
    for inst in machines:
        inst.bridge.call("net_sim", latency_ms=45, jitter_ms=15, loss=0.01)

    # Reproduce the state exposed by attachment invalidation: the generated
    # sword is still physically held, but its script sheath slot is empty.
    # Re-arming must repair that same object, never allocate another copy.
    proxy = human(host, guest_id)
    code(host, f'ReadCharacterID({proxy["id"]}).SetArrayIntVar("weapon_slots",3,-1); CoopArm({proxy["id"]});')
    assert len(state(host)["items"]) == 2, "repairing a held loadout copy spawned a duplicate sword"
    consistent("loadout race repaired", 2)

    # Actual campaign all-human deaths invoke the five-second team retry.
    # Each round uses gameplay draw/sheathe functions and inspects real rig
    # attachments, not just the AngelScript inventory array.
    for round_index in range(3):
        for inst in machines:
            actor = inst.bridge.me()["id"]
            code(inst, 'UnSheathe(_held_right,_sheathed_right);', actor)
        drawn = consistent("swords drawn on both peers", 2)
        assert all(v["slots"][1] == identities[p] and v["attachments"][0]["state"] == 0 for p, v in drawn.items())
        for inst in machines:
            code(inst, 'Sheathe(_held_right,_sheathed_right); UnSheathe(_held_right,_sheathed_right);', inst.bridge.me()["id"])
        consistent("repeat draw keeps one physical weapon", 2)
        host.bridge.call("level_messages", prefix="reset")
        for inst in machines:
            code(inst, 'SetKnockedOut(_dead); Ragdoll(_RGDL_LIMP);', inst.bridge.me()["id"])
        wait(lambda: any(m == "reset" for m in host.bridge.call("level_messages", prefix="reset")["messages"]),
             "team death triggers campaign reset", 30)
        _skip(host)
        wait(lambda: all(inst.bridge.me().get("knocked_out") == 0 for inst in machines), "all humans respawn")
        time.sleep(4.0)  # production loadout repair delay, including a late reset message
        restored = consistent("respawn restores original weapons", 2)
        assert {p: v["attachments"][0]["common_id"] for p, v in restored.items()} == identities, restored
        assert all(len(v["attachments"]) == 1 for v in restored.values())
        metrics["rounds"].append({"drawn": drawn, "restored": restored})

    # A runtime pickup has no editor/reset connection. Its reliable detach
    # must still remove the physical attachment on host and replicas, and
    # leave the thrown/dropped object in the world instead of deleting it.
    hero = human(host, host_id)
    code(host, 'int w=CreateObject("Data/Items/DogWeapons/DogSword.xml",true); '
               f'ReadObjectFromID(w).SetTranslation(ReadCharacterID({hero["id"]}).position); '
               f'ReadCharacterID({hero["id"]}).Execute("this_mo.AttachItemToSlot("+w+",_at_grip,false); '
               'HandleEditorAttachment("+w+",_at_grip,false);");')
    wait(lambda: len(human(host, host_id)["attachments"]) == 2 and len(human(guest, host_id)["attachments"]) == 2,
         "runtime hand weapon is replicated")
    hero = human(host, host_id)
    runtime_id = hero["weapon_slots"][1]
    runtime_common = hero["weapon_slots_common"][1]
    assert not any(c["id"] == runtime_id for c in hero["item_connections"]), "fixture is an editor attachment"
    code(host, f'ReadObjectFromID({hero["id"]}).Disconnect(ReadObjectFromID({runtime_id}));')
    wait(lambda: all(next(it for it in state(inst)["items"] if it["common_id"] == runtime_common)["held_by"] == -1
                     for inst in machines), "reliable runtime detach reaches both peers")
    consistent("runtime detach removes all physical copies", 3)
    for inst in machines:
        dropped = next(it for it in state(inst)["items"] if it["common_id"] == runtime_common)
        assert dropped["held_by"] == -1 and dropped["parent_id"] == -1, dropped
    metrics["dropped_item"] = runtime_common

    # Owner-simulated ThrowWeapon must remain a legitimate free world item.
    actor = guest.bridge.me()["id"]
    code(guest, 'UnSheathe(_held_right,_sheathed_right); ThrowWeapon(_held_right);', actor)
    wait(lambda: all(next(it for it in state(inst)["items"] if it["common_id"] == identities[guest_id])["held_by"] == -1
                     for inst in machines), "guest throw reaches host")
    assert all(len(state(inst)["items"]) == 3 for inst in machines)
    # Re-arm through the normal co-op backend; it reclaims the released copy.
    wait(lambda: identities[guest_id] not in human(host, guest_id)["weapon_slots_common"],
         "owner drop inventory reaches host before re-arm")
    proxy = human(host, guest_id)
    code(host, f'CoopArm({proxy["id"]});')
    wait(lambda: all(identities[guest_id] in human(inst, guest_id)["weapon_slots_common"] for inst in machines),
         "owner receives reclaimed loadout copy")
    consistent("thrown loadout item reused", 3)

    # A sword and its separate scabbard share a hip. The scabbard's ObjectID
    # is not an inventory index; repeated attachment must preserve both items
    # exactly once through draw/sheath transitions.
    proxy = human(host, guest_id)
    code(host, 'int s=CreateObject("Data/Items/DogWeapons/DogSwordScabbard.xml",true); '
               f'ReadObjectFromID(s).SetTranslation(ReadCharacterID({proxy["id"]}).position); '
               f'ReadObjectFromID({proxy["id"]}).AttachItem(ReadObjectFromID(s),_at_sheathe,true); '
               f'ReadObjectFromID({proxy["id"]}).AttachItem(ReadObjectFromID(s),_at_sheathe,true);')
    scabbards = consistent("sword and separate scabbard replicate", 4)
    assert scabbards[guest_id]["slots"][3] == identities[guest_id] and scabbards[guest_id]["slots"][5] >= 0, scabbards
    code(guest, 'UnSheathe(_held_right,_sheathed_right);', guest.bridge.me()["id"])
    drawn = consistent("drawing leaves only the scabbard on the hip", 4)
    assert drawn[guest_id]["slots"][1] == identities[guest_id] and drawn[guest_id]["slots"][3] != identities[guest_id], drawn
    code(guest, 'Sheathe(_held_right,_sheathed_right);', guest.bridge.me()["id"])
    consistent("sheathed sword and scabbard remain distinct", 4)
    metrics["scabbard_draw"] = drawn

    late = ctx.instance("inventory_late", keys={"mp_port": "27946"})
    machines.append(late)
    late.bridge.call("connect", ip="127.0.0.1:27944")
    _ready(late)
    wait(lambda: len(rows(late)) == 3 and len(state(late)["items"]) == 5, "late join receives current weapon world", 45)
    final = consistent("late join has current attachments only", 5)
    assert all(item in [attachment["common_id"] for attachment in final[p]["attachments"]]
               for p, item in identities.items()), final
    for inst in machines:
        free = next(it for it in state(inst)["items"] if it["common_id"] == runtime_common)
        assert free["held_by"] == -1, free
    metrics["late_join"] = final
    errors = [error for inst in machines for error in script_errors(inst)]
    assert not errors, errors[:6]
    return {"passed": True, "metrics": metrics, "notes": []}
