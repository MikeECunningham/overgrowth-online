"""Exercise independent XP sources, pooled damage loss, defeat and Arena score transitions."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP, _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows, _skip
from .scenarios import script_errors


def progression_rewards(ctx):
    host = ctx.instance("xp_event_rewards", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28766", "player_invincible": "true", "tutorials": "false",
        "difficulty_scaling": "false", "oo_arena_random": "false"})
    bridge = host.bridge

    def wallet():
        view = bridge.call("progression", actor=owner)["view"]
        return view["held"], view["pooled"]

    def rolls():
        return bridge.call("progression", actor=owner)["ledger"]["reward_generation"]

    def level(code):
        _code(host, code, "level")

    def settings(**values):
        for key, value in values.items():
            assert _probe(host, f'LobbySetModeSetting("progression_{key}",{value})') == "true", key

    def phase(value):
        return _probe(host, "int(phase)", "level", "level_specific") == str(value)

    def wait(fn, label):
        return bridge.wait(fn, 45, step=.1, what=label)

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);')
    settings(gain_damage=1, gain_kill=1, loss_damage=1, loss_death=1, gain_checkpoint=1, free_buffs=1)
    _code(host, 'LobbyStart();')
    _ready(host, FIRST)
    _skip(host)
    owner = next(p["id"] for p in _rows(host) if not p["bot"])
    hero = bridge.me()["id"]
    pos = bridge.me()["pos"]
    marker = bridge.call("spawn", path="Data/Objects/placeholder/placeholder_player_spawn.xml", pos=pos)["object_id"]
    controllers = []
    for controller in ("therium/Hotspots/goal_level", "Hotspots/overgrowth_level"):
        goal = bridge.call("spawn", path=f"Data/Objects/{controller}.xml", pos=pos)["object_id"]
        controllers.append(goal)
        before = wallet()
        rolls_before = rolls()
        level(f'Object@ o = ReadObjectFromID({goal}); ScriptParams@ p = o.GetScriptParams();'
              'p.SetString("goal_0","reach 17");p.SetString("goal_1","reach 18");'
              f'p.SetString("player_spawn","{marker} {hero}");o.ReceiveScriptMessage("player_entered_checkpoint 17 {hero}");')
        wait(lambda: wallet()[0] == sum(before) + 25, f"authored {controller} checkpoint")
        level(f'ReadObjectFromID({goal}).ReceiveScriptMessage("player_entered_checkpoint 18 {hero}");')
        wait(lambda: _probe(host, f"cast<Hotspot@>(ReadObjectFromID({goal})).GetIntVar(\"progress\")", "level") == "2",
             "objective without a valid respawn marker advances")
        assert wallet()[0] == sum(before) + 25, "a non-spawn objective awarded checkpoint XP"
        assert rolls() == rolls_before + 1, "only the objective with a real checkpoint should roll"
    for goal in controllers:
        level(f"QueueDeleteObjectID({goal});")
    enemy = bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                        pos=[pos[0]+4, pos[1]+.25, pos[2]+3], teams="xp_enemy")["object_id"]
    _code(host, "this_mo.static_char=true;", enemy)
    held, pooled = wallet()
    level(f"ProgressionReportHit({enemy},{hero},.25f,false);")
    damage = wallet()[1] - pooled
    assert damage > 0 and wallet()[0] == held, wallet()
    level(f"ProgressionReportHit({enemy},{hero},0,true);ProgressionReportHit({enemy},{hero},0,true);")
    assert wallet() == (held, pooled + damage * 5), wallet()
    level("ProgressionCheckpoint(7);ProgressionCheckpoint(7);ProgressionCheckpoint();")
    banked, pool = wallet()
    assert (banked, pool) == (held + pooled + damage * 5 + 25, 0), wallet()
    level(f"ProgressionResetAttempt();ProgressionReportHit({enemy},{hero},.25f,false);"
          f"ProgressionReportHit({hero},{enemy},.25f,false);")
    remaining, pending = wallet()
    assert remaining == banked and math.isclose(pending, damage * .6), wallet()
    level(f"ProgressionReportHit({hero},{enemy},1.f,false);")
    assert wallet() == (banked, 0), "Damage in excess of the pool must leave the bank intact"
    level(f"ProgressionReportHit({enemy},{hero},.25f,false);")
    bounty = damage * 4
    # A partial pool pays first; duplicate reports for the same down cost nothing.
    level(f"ProgressionReportHit({hero},{enemy},0,true);ProgressionReportHit({hero},{enemy},0,true);")
    after_partial = banked - (bounty - damage)
    assert wallet() == (after_partial, 0), wallet()
    level(f"ProgressionReportRecovery({hero});ProgressionReportHit({enemy},{hero},1.f,false);"
          f"ProgressionReportHit({hero},{enemy},0,true);")
    assert wallet() == (after_partial, 0), "A pool equal to the death cost must protect the entire bank"
    # A real knockout consumes one bounty; its leftover pool expires on retry.
    level(f"ProgressionReportRecovery({hero});ProgressionReportHit({enemy},{hero},1.f,false);"
          f"ProgressionReportHit({enemy},{hero},.25f,false);")
    _code(host, f"attacked_by_id={enemy};SetKnockedOut(_unconscious);SetKnockedOut(_dead);", hero)
    assert wallet() == (after_partial, damage), wallet()
    level('level.SendMessage("reset");')
    wait(lambda: wallet() == (after_partial, 0) and bridge.me()["knocked_out"] == 0, "retry clears only the leftover pool")
    _code(host, "attacked_by_id=-1;SetKnockedOut(_dead);", bridge.me()["id"])
    after_repeat, pending = wallet()
    assert 0 <= after_repeat < after_partial and pending == 0, "A second death with no pool must hit the bank"
    before_shop = sum(wallet())
    level("ProgressionBeginShop(false);")
    assert wallet() == (before_shop, 0), "The shop banks XP without paying a checkpoint bonus"

    level("LobbyEndGame();")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "lobby")
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);'
          'LobbySetModeSetting("progression",1);LobbySetForceTeams(true);')
    bot = bridge.call("session_slots", action="add")["created"]
    _code(host, f"LobbySetPlayerTeam({owner},1);LobbySetPlayerTeam({bot},2);")
    settings(gain_damage=1, gain_kill=0, loss_damage=1, loss_death=0, gain_round=1, gain_match=1, free_buffs=1)
    _code(host, 'LobbyStart();')
    _ready(host, ARENA_MAP)
    wait(lambda: phase(2), "first fight")
    _code(host, "max_points=2;", "level", "level_specific")
    starting_balance = wallet()[0]
    initial_rolls = rolls()
    combat_earned = 0
    for score in (1, 2):
        enemy = next(p["avatar_id"] for p in _rows(host) if p["id"] == bot)
        hero = bridge.me()["id"]
        held_before, pool_before = wallet()
        level(f"ProgressionReportHit({enemy},{hero},.5f,false);")
        earned = wallet()[1] - pool_before
        assert earned > 0 and wallet()[0] == held_before
        level(f"ProgressionReportHit({hero},{enemy},.5f,false);")
        assert math.isclose(wallet()[1], pool_before + earned * .6) and wallet()[0] == held_before
        before_defeat = wallet()
        level(f"ProgressionReportHit({hero},{enemy},0,true);ProgressionReportRecovery({hero});")
        assert wallet() == before_defeat, "Disabling defeat loss should preserve both balances"
        combat_earned += wallet()[1]
        _code(host, f"attacked_by_id={hero};SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);", enemy)
        wait(lambda: phase(3), "winning round")
        assert math.isclose(wallet()[0], starting_balance + combat_earned + score * 25) and wallet()[1] == 0, wallet()
        assert rolls() == initial_rolls + score
        if score == 1:
            wait(lambda: phase(2), "second fight")
    wait(lambda: phase(4), "winning match")
    assert math.isclose(wallet()[0], starting_balance + combat_earned + 150) and wallet()[1] == 0, wallet()
    assert rolls() == initial_rolls + 2, "the deciding round should roll exactly once"
    arena_won_xp = wallet()[0] - starting_balance

    level("LobbyEndGame();")
    wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"], "Arena lobby")
    settings(shared=1, loss_damage=0, loss_death=1)
    _code(host, "LobbyStart();")
    _ready(host, ARENA_MAP)
    wait(lambda: phase(2), "Arena round with shared defeat loss")
    _code(host, "max_points=2;", "level", "level_specific")
    enemy = next(p["avatar_id"] for p in _rows(host) if p["id"] == bot)
    hero = bridge.me()["id"]
    shared_bank, shared_pool = wallet()
    assert shared_pool == 0
    level(f"ProgressionReportHit({enemy},{hero},1.f,false);ProgressionReportHit({enemy},{hero},.5f,false);")
    earned = wallet()[1]
    assert earned > 0 and wallet()[0] == shared_bank, "Defeat loss alone must keep Arena combat XP pooled"
    level(f"ProgressionReportHit({hero},{enemy},1.f,false);")
    assert wallet() == (shared_bank, earned), "The ordinary damage penalty is off"
    level(f"ProgressionReportHit({hero},{enemy},0,true);")
    assert wallet()[0] == shared_bank and math.isclose(wallet()[1], earned / 3), wallet()
    _code(host, f"attacked_by_id={hero};SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);", enemy)
    wait(lambda: phase(3), "shared Arena round banks the surplus after defeat")
    assert math.isclose(wallet()[0], shared_bank + earned / 3 + 25) and wallet()[1] == 0, wallet()
    wait(lambda: phase(2), "next shared Arena round")
    enemy = next(p["avatar_id"] for p in _rows(host) if p["id"] == bot)
    hero = bridge.me()["id"]
    shared_bank = wallet()[0]
    level(f"ProgressionReportHit({enemy},{hero},.5f,false);")
    half_bounty = wallet()[1]
    assert 0 < half_bounty < shared_bank
    _code(host, f"attacked_by_id={enemy};SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);", hero)
    wait(lambda: phase(3), "real Arena defeat")
    assert math.isclose(wallet()[0], shared_bank - half_bounty) and wallet()[1] == 0, wallet()
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"campaign_damage_xp": damage, "death_bounty": bounty,
            "partial_pool_bank_loss": banked - after_partial, "empty_pool_bank_loss": after_partial - after_repeat,
            "arena_won_xp": arena_won_xp, "shared_arena_surplus_banked": earned / 3}, "notes": []}


def checkpoint_reward_slots(ctx):
    host, guest = [ctx.instance(f"checkpoint_{name}", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": str(28778 + i), "tutorials": "false", "player_invincible": "true"})
        for i, name in enumerate(("host", "guest"))]

    def wait(fn, what):
        return host.bridge.wait(fn, 45, step=.1, what=what)

    def start():
        _code(host, "LobbyStart();")
        for peer in (host, guest):
            _ready(peer, FIRST)
        _skip(host)

    def data(peer, seat, **action):
        return peer.bridge.call("progression", actor=seat, **action)

    def queues():
        ledger = data(host, owner)["ledger"]
        return {p["id"]: p.get("reward_hands", []) for p in ledger["players"]}

    def level(code):
        _code(host, code, "level")

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_free_buffs",1);'
          'LobbySetModeSetting("progression_gain_checkpoint",0);LobbySetModeSetting("progression_timer",0);')
    guest.bridge.call("connect", ip="127.0.0.1:28778")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "both human slots")
    owner, visitor = [p["id"] for p in sorted(_rows(host), key=lambda p: not p["host"])]
    start()
    # Stop as soon as both have a backlog and at least one roll differed.
    for step in range(1, 257):
        level(f"ProgressionCheckpoint({step}, 1000123);")
        hands = queues()
        rolls = [{h["generation"] for h in hands.get(p, [])} for p in (owner, visitor)]
        if min(map(len, rolls)) >= 2 and rolls[0] != rolls[1]:
            break
    else:
        raise AssertionError("expected distinct, independently rolled checkpoint hands")
    generation = data(host, owner)["ledger"]["reward_generation"]
    level(f"ProgressionCheckpoint({step},1000123);ProgressionCheckpoint();ProgressionResetAttempt();"
          f"ProgressionCheckpoint({step},1000123);")
    assert data(host, owner)["ledger"]["reward_generation"] == generation and queues() == hands
    for peer, seat in ((host, owner), (guest, visitor)):
        wait(lambda: data(peer, seat)["view"]["reward_pending"], "offered an in-play hand")
        view = data(peer, seat)["view"]
        assert len({card["id"] for card in view["reward_cards"]}) == 4 and not view["shop_open"]
    key = data(host, owner)["view"]["reward_cards"][0]["id"]
    token = hands[owner][0]["generation"]
    assert not data(guest, owner, action="reward", key=key)["submitted"]
    assert data(host, owner, action="reward", key=key, generation=token)["submitted"]
    assert not data(host, owner, action="reward", key=key, generation=token)["submitted"]
    assert queues()[owner] == hands[owner][1:] and queues()[visitor] == hands[visitor]
    owed = queues()

    level("ProgressionBeginShop(false);")
    wait(lambda: data(guest, visitor)["view"]["shop_open"], "paid shop opens with pending free choices")
    for peer, seat in ((host, owner), (guest, visitor)):
        assert data(peer, seat, action="ready")["submitted"]
    wait(lambda: not data(host, owner)["view"]["shop_open"], "paid shop closes independently")
    assert queues() == owed and data(host, owner)["ledger"]["reward_generation"] == generation
    assert _probe(host, "SessionSave(2)", "level") == "true"
    wait(lambda: _probe(host, "SessionSaveExists(2)", "level") == "true", "checkpoint ledger saved")
    assert _probe(host, "SessionLoad(2)", "level") == "true"
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)), "saved lobby reopens")
    start()
    wait(lambda: queues() == owed and data(guest, visitor)["view"]["reward_generation"] == owed[visitor][0]["generation"],
         "restored both slots and the original guest hand")
    level(f"ProgressionCheckpoint({step},1000123);")
    assert data(host, owner)["ledger"]["reward_generation"] == generation
    view = data(guest, visitor)["view"]
    assert data(guest, visitor, action="reward", key=view["reward_cards"][0]["id"])["submitted"]
    wait(lambda: queues()[visitor] == owed[visitor][1:], "guest claimed their saved hand exactly once")
    assert all(not script_errors(p) for p in (host, guest))
    return {"passed": True, "metrics": {"checkpoints": step, "awards": list(map(len, rolls))}, "notes": []}
