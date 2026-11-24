"""A living enemy's factors stay fixed on both peers until a real campaign save or retry."""
import math
import time

from .campaign_scaling_suite import RENDER_KEYS, FACTORS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _skip
from .scenarios import script_errors


def enemy_difficulty_boundaries(ctx):
    host = ctx.instance("frozen_stats_host", keys={**RENDER_KEYS, "mp_port": "28543", "difficulty_scaling": "true",
                        "game_difficulty": "0.5", "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false",
                        "campaign_mp_scaling": "false", "campaign_ai_scaling": "true", "player_invincible": "true"})
    guest = ctx.instance("frozen_stats_guest", keys={**RENDER_KEYS, "mp_port": "28544"})
    peers = (host, guest)

    def wait(predicate, message):
        return host.bridge.wait(predicate, 35, step=.12, what=message)

    def challenge(peer):
        return peer.bridge.status()["online"]["challenge"]

    def combat(peer, shared=363):
        actor = next((c for c in peer.bridge.state() if c["common_id"] == shared), None)
        return actor["combat_stats"] if actor else {}

    def factors(peer, shared=363):
        return tuple(combat(peer, shared).get(name) for name in FACTORS)

    def at_pressure(peer, value, shared=363):
        v = factors(peer, shared)[0]
        return v is not None and math.isclose(v, 1 + value, abs_tol=.00051)

    def both_at(value):
        return all(at_pressure(p, value) for p in peers)

    def win(key):
        _code(host, f'Online_ChallengeEncounterBegin("{key}");Online_ChallengeEncounterWin("{key}");')

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("challenge_performance",1);LobbySetModeSetting("challenge_performance_rate",2);', 'menu')
    guest.bridge.call("connect", ip="127.0.0.1:28543")
    wait(lambda: all(len(p.bridge.status()["online"]["players"]) == 2 for p in peers), "two humans in lobby")
    _code(host, 'LobbyStart();', 'menu')
    for p in peers:
        _ready(p, FIRST)
    _skip(host)
    wait(lambda: all(challenge(p).get("enemy_difficulty", {}).get("humans") == 2 for p in peers), "replicated load sample")
    wait(lambda: both_at(0), "ordinary opening enemy")
    old = factors(host)
    assert all(combat(p)["campaign_perception_mult"] == 1 for p in peers)
    hero = host.bridge.me()
    pos = [hero["pos"][0]+3, hero["pos"][1]+.25, hero["pos"][2]+2]
    def guard():
        actor = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                                 pos=pos, teams="frozen_enemy")["object_id"]
        _code(host, 'this_mo.static_char=true;combat_allowed=false;chase_allowed=false;always_unaware=true;', actor)
        return actor
    def actor_ready(actor):
        return all(combat(p, actor).get("campaign_base_difficulty") == .5 for p in peers)
    def bounty(actor):
        seat = next(r["id"] for r in host.bridge.status()["online"]["players"] if r["host"])
        def pooled():
            return host.bridge.call("progression",actor=seat)["view"]["pooled"]
        before = pooled()
        _code(host, f'ProgressionReportHit({actor},{hero["id"]},0,true);')
        return pooled()-before
    original = guard()
    wait(lambda: actor_ready(original), "fresh NPC gets host's sampled base")
    early_xp = bounty(original)
    assert early_xp > 0
    _code(host, 'SetConfigValueString("difficulty_preset","Expert");')

    _code(host, 'LobbySetModeSetting("challenge_time",3);')
    for i in range(16):
        win(f"frozen_enemy_win_{i}")
    wait(lambda: challenge(host)["pressure"] >= .641, "time and victory accumulate in the ledger")
    _code(host, 'LobbySetModeSetting("challenge_time",0);LobbyAddBot();')
    pending = challenge(host)["pressure"]
    # Allow at least three ordinary enemy director polls.
    time.sleep(1.2)
    assert all(factors(p) == old for p in peers), "pressure changed fighting stats before checkpoint"
    assert all(combat(p)["campaign_perception_mult"] == 1 for p in peers), "perception changed before checkpoint"
    assert all(combat(p)["campaign_base_difficulty"] == .5 and combat(p)["game_difficulty"] == .5 for p in peers)
    # A new guard inherits the active sample. Use a fixed scene identity for the
    # sparse AI trait so coverage is independent of dynamic ID allocation.
    fresh = guard()
    trait_actor = 21
    _code(host, f'ReadObjectFromID({trait_actor}).GetScriptParams().SetString("Teams","frozen_enemy");')
    wait(lambda: actor_ready(fresh) and actor_ready(trait_actor), "later enemies inherit the active base")
    assert all(combat(p, trait_actor)["campaign_ai_bonus"] == 0 for p in peers)
    assert bounty(fresh) == early_xp, "changing base preset, pressure and party mid-fight repriced XP"
    assert challenge(host)["enemy_difficulty"]["bots"] == 0
    assert host.bridge.status()["difficulty_hud"]["progression"]["applied_pressure"] == 0
    # First save marker in White Flags. Drive the ordinary authored reach event.
    _code(host, 'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint 0 0 477");')
    wait(lambda: both_at(pending), "save applies accumulated difficulty on host and guest")
    wait(lambda: all(combat(p)["campaign_perception_mult"] > 1.08 for p in peers), "saved perception reaches both peers")
    wait(lambda: all(challenge(p)["enemy_difficulty"]["bots"] == 1 for p in peers), "roster sampled at checkpoint")
    wait(lambda: all(combat(p)["campaign_base_difficulty"] == 1 and combat(p)["game_difficulty"] == 1 and
                     math.isclose(combat(p,trait_actor)["campaign_ai_bonus"],.01,abs_tol=1e-6) for p in peers),
         "checkpoint applies changed AI preset and sparse trait on both peers")
    higher = guard()
    wait(lambda: all(at_pressure(p,pending,higher) for p in peers) and
                 combat(host,higher).get("campaign_base_difficulty") == 1, "new checkpoint fighter")
    late_xp = bounty(higher)
    assert late_xp > early_xp, (late_xp,early_xp)
    _skip(host)
    win("frozen_retry_win")
    wait(lambda: challenge(host)["pressure"] > pending+.039, "another banked win")
    after = challenge(host)["pressure"]
    _code(host, 'level.SendMessage("reset");')
    wait(lambda: both_at(after), "accepted checkpoint retry samples again")
    frozen_perception = combat(host)["campaign_perception_mult"]
    retried = factors(host)[0]
    _skip(host)
    # Turning the master off also waits for a safe boundary. Traditional mode
    # has a base preset of its own, and must not read live edits during combat.
    _code(host, 'SetConfigValueString("difficulty_scaling","false");'
                'SetConfigValueString("difficulty_preset","Hardcore");')
    time.sleep(1.2)
    assert both_at(after), "disabling scaling retuned a living enemy"
    assert all(combat(p)["game_difficulty"] == 1 for p in peers)
    assert all(combat(p)["campaign_perception_mult"] == frozen_perception for p in peers)
    _code(host, 'level.SendMessage("reset");')
    wait(lambda: both_at(0) and all(combat(p)["campaign_base_difficulty"] == .5 and
                                  combat(p)["game_difficulty"] == .5 and
                                  combat(p)["campaign_perception_mult"] == 1 for p in peers),
         "retry applies Traditional mode and the selected base")
    _skip(host)
    _code(host, 'SetConfigValueString("difficulty_preset","Expert");')
    time.sleep(1.2)
    assert all(combat(p)["game_difficulty"] == .5 for p in peers), "Traditional base changed mid-fight"
    traditional = guard()
    wait(lambda: actor_ready(traditional), "Traditional late spawn inherits the frozen base")
    _code(host, 'level.SendMessage("reset");')
    wait(lambda: both_at(0) and all(combat(p)["campaign_base_difficulty"] == 1 and
                                  combat(p)["game_difficulty"] == 1 for p in peers),
         "Traditional retry applies the newly selected base")
    assert all(not script_errors(p) for p in peers), [script_errors(p) for p in peers]
    return {"passed": True, "metrics": {"loaded": old[0], "saved": 1+pending,
                                          "retried": retried, "traditional_base": combat(host)["game_difficulty"],
                                          "guest": factors(guest)[0],
                                          "xp_before_save": early_xp, "xp_after_save": late_xp}, "notes": []}
