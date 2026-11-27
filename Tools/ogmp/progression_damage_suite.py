"""Measure physical blunt hits, cuts and delayed bleeding for each attacker on both owners."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows, _skip
from .scenarios import script_errors


def progression_damage(ctx):
    host = ctx.instance("damage_host", keys={**RENDER_KEYS, "mp_port": "29917"})
    guest = ctx.instance("damage_guest", keys={**RENDER_KEYS, "mp_port": "29918"})
    peers = (host, guest)

    def wait(fn, label):
        return host.bridge.wait(fn, 45, step=.1, what=label)

    _code(host, "LobbyBeginHost();")
    guest.bridge.call("connect", ip="127.0.0.1:29917")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "two players")
    owner = next(row["id"] for row in _rows(host) if row["host"])
    visitor = next(row["id"] for row in _rows(host) if not row["host"])
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);'
          'LobbySetModeSetting("progression_gain_checkpoint",1);LobbySetModeSetting("progression_timer",0);LobbyStart();')
    for peer in peers:
        _ready(peer, FIRST)
    _skip(host)
    _code(host, "ProgressionCheckpoint(1,912);ProgressionBeginShop(false);", "level")
    wait(lambda: all(peer.bridge.call("progression",actor=seat)["view"].get("shop_open")
                     for peer,seat in zip(peers,(owner,visitor))), "both shops")
    assert host.bridge.call("progression", actor=owner, action="buy", key="bone_damage")["submitted"]
    assert guest.bridge.call("progression", actor=visitor, action="buy", key="flesh_damage")["submitted"]

    def synced(peer):
        rows = peer.bridge.call("progression", actor=visitor)["ledger"]["players"]
        return {r["id"]:(r["bone_damage"],r["flesh_damage"]) for r in rows} == {owner:(1,0),visitor:(0,1)}
    wait(lambda: all(synced(p) for p in peers), "both owners see the independent purchases")

    for peer, victim_seat in zip(peers,(owner,visitor)):
        bodies = {row["id"]:row["avatar_id"] for row in _rows(peer)}
        victim = bodies[victim_seat]
        plain = next(c["id"] for c in peer.bridge.state() if c["id"] not in bodies.values())
        peer.bridge.call("level_messages",prefix="damage_sample ")
        # A glancing thrown blade (type 1) inflicts blunt trauma and blood loss
        # on the same contact. Freeze velocity, shields and attribution per hit.
        _code(peer, 'int knife=CreateObject("Data/Items/rabbit_weapons/rabbit_knife.xml",true);'
              'ReadObjectFromID(knife).SetTranslation(this_mo.position+vec3(0,25,0));'
              f'array<int> sources={{{plain},{bodies[owner]},{bodies[visitor]}}};'
              'attack_getter2.Load("Data/Attacks/frontkick.xml");'
              'for(uint n=0;n<sources.size();++n){int source=sources[n];'
              'invincible=false;ko_shield=0;temp_health=0.5f;permanent_health=blood_health=1.0f;'
              'this_mo.velocity=vec3(0);ReadItemID(knife).last_held_char_id_=source;'
              'ReadItemID(knife).SetLinearVelocity(vec3(2,0,0));'
              'ResolveItemImpact("metal",this_mo.position,knife,1);'
              'float blunt=0.5f-temp_health, blood=1.0f-blood_health;'
              'float before=blood_health;TakeSharpDamage(0.1f,this_mo.position,source,false);'
              'blood_damage=0;attacked_by_id=source;TakeDelayedBloodDamage(0.1f);'
              'level.SendMessage("damage_sample "+blunt+" "+blood+" "+(before-blood_health)+" "+blood_damage);}'
              'Recover();QueueDeleteObjectID(knife);', victim)
        lines = peer.bridge.call("level_messages",prefix="damage_sample ")["messages"]
        assert len(lines)==3, lines
        base, bone, flesh = [[float(s) for s in line.split()[1:]] for line in lines]
        assert all(value > 0 for value in base), base
        for choice, multipliers in ((bone,(1.12,1,1,1)), (flesh,(1,1.12,1.12,1.12))):
            assert all(math.isclose(value, ref*scale, rel_tol=.002, abs_tol=.00001)
                       for value,ref,scale in zip(choice,base,multipliers)), (base,bone,flesh)
    assert not any(script_errors(peer) for peer in peers)
    return {"passed":True, "metrics":{"bone_ratio":1.12,"flesh_ratio":1.12,"tested_simulating_owners":2}, "notes":[]}
