"""Verify independent defensive purchases and the original attack multiplier on both peers."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe
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
    _code(host, "ProgressionCheckpoint(1,912);ProgressionCheckpoint(2,912);ProgressionBeginShop(false);", "level")
    wait(lambda: all(peer.bridge.call("progression",actor=seat)["view"].get("shop_open")
                     for peer,seat in zip(peers,(owner,visitor))), "both shops")
    def action(peer, seat, command, key=""):
        return peer.bridge.call("progression", actor=seat, action=command, key=key)["submitted"]

    assert action(host, owner, "ready")
    assert action(host, owner, "buy", "bone_health")
    assert not host.bridge.call("progression", actor=owner)["view"]["ready"]
    assert action(guest, visitor, "buy", "flesh_health")
    for peer, seat, ranks in ((host, owner, (1, 0)), (guest, visitor, (0, 1))):
        victim = next(row["avatar_id"] for row in _rows(peer) if row["id"] == seat)
        wait(lambda: _probe(peer, 'progression_health+" "+progression_flesh_health', victim)
             == f"{ranks[0]} {ranks[1]}", "defensive ranks applied on simulating owner")
        _code(peer, 'invincible=false;ko_shield=0;temp_health=blood_health=permanent_health=1;'
              'blood_damage=0;TakeDamage(.1f);TakeBloodDamage(.1f);TakeDelayedBloodDamage(.1f);'
              'float base=.1f*p_damage_multiplier*equipped_damage_factor;'
              'level.SendMessage("damage_sample "+((1-temp_health)/base)+" "+((1-blood_health)/base)+" "+(blood_damage/base));', victim)
        values = [float(v) for v in peer.bridge.call("level_messages",prefix="damage_sample ")["messages"][-1].split()[1:]]
        for value, rank in zip(values, (ranks[0], ranks[1], ranks[1])):
            assert math.isclose(value, 1/(1+.16*rank), abs_tol=.0001), (values, ranks)
    attacker = next(row["avatar_id"] for row in _rows(host) if row["id"] == owner)
    before = float(_probe(host, "p_attack_damage_mult", attacker))
    assert action(host, owner, "buy", "damage")
    wait(lambda: math.isclose(float(_probe(host, "p_attack_damage_mult", attacker)), before*1.12, rel_tol=.0001),
         "common Damage rank increases the authored attack multiplier")
    assert not any(script_errors(peer) for peer in peers)
    return {"passed":True, "metrics":{"tested_simulating_owners":2}, "notes":[]}
