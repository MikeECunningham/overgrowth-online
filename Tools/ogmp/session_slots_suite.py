"""Persistent unit slots, designated/automatic admission, physical occupant swaps and staged restore."""
import copy
import time

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _rows, _skip
from .mode_settings_suite import _probe as menu_probe
from .scenarios import script_errors


def session_slots(ctx):
    host = ctx.instance("slot_host", width=1280, height=720, keys={
        **RENDER_KEYS, "mp_port": "28671", "difficulty_scaling": "false", "player_invincible": "true"})
    guest = ctx.instance("slot_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "28672"})
    replacement = ctx.instance("slot_replacement", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "28673"})
    wait = lambda fn, label, seconds=35: host.bridge.wait(fn, seconds, step=.1, what=label)

    def data(peer=host, **kw):
        return peer.bridge.call("session_slots", **kw)

    def saved(peer=host):
        return data(peer)["saved"]

    def ids(peer=host):
        return {p["id"] for p in _rows(peer)}

    def slots(peer=host):
        return {p["id"]: p for p in saved(peer)["slots"]}

    def action(name, peer=host, **kw):
        response = data(peer, action=name, **kw)
        assert response["submitted"], (name, kw, response)
        return response

    def add(player=False):
        id = action("add")["created"]
        assert slots()[id]["kind"] == "bot", "new slots must start as bots"
        if player:
            action("kind", slot=id, player=True)
            assert id not in ids() and slots()[id]["kind"] == "player"
        return id

    def press_pad(peer, pad, expected=None):
        peer.bridge.call("virtual_pad", action="attach", slot=pad)
        time.sleep(.15)
        peer.bridge.call("virtual_pad", action="button", slot=pad, button=0, down=True)
        time.sleep(.2)
        peer.bridge.call("virtual_pad", action="button", slot=pad, button=0, down=False)
        if expected is not None:
            wait(lambda: peer.bridge.status()["local_players"] == expected, "couch player admitted")

    _code(host, 'LobbyBeginHost();this_ui.SendCallback("multiplayer_lobby.as");', "menu")
    original_host = next(p["id"] for p in _rows(host) if p["host"])
    couch = add(player=True)
    identity = copy.deepcopy(slots())
    action("admission", automatic=False)
    press_pad(host, 0, 2)
    wait(lambda: couch in ids() and next(p for p in _rows(host) if p["id"] == couch)["local_index"] == 1,
         "couch controller claims its designated slot")
    press_pad(host, 1)
    time.sleep(.5)
    assert host.bridge.status()["local_players"] == 2 and len(slots()) == 2
    # No available human placeholder: a bot must not be silently consumed.
    bot = add()
    guest.bridge.call("connect", ip="127.0.0.1:28671")
    wait(lambda: not guest.bridge.status()["online"]["active"], "remote connection rejected without a player slot")
    assert len(ids()) == 3 and bot in ids()
    action("kind", slot=bot, player=True)
    guest.bridge.call("connect", ip="127.0.0.1:28671")
    wait(lambda: bot in ids(guest) and len(ids()) == 3, "remote human fills designated former bot slot")
    guest.bridge.call("menu", script="multiplayer_lobby.as")
    remote_couch = add(player=True)
    wait(lambda: remote_couch in slots(guest), "new designated slot replicated")
    press_pad(guest, 0, 2)
    wait(lambda: remote_couch in ids() and len(ids()) == 4, "remote couch fills designated slot")
    press_pad(guest, 1)
    time.sleep(.5)
    assert guest.bridge.status()["local_players"] == 2 and len(slots()) == 4
    assert not data(guest, action="admission", automatic=True)["submitted"]
    assert not data(guest, action="swap", first=original_host, second=bot)["submitted"]
    assert not saved()["auto_admit"]
    assert slots()[couch]["uid"] == identity[couch]["uid"]
    _code(host, 'SetConfigValueInt("controller_menu_owner",0);', "menu")
    ctx.shot(host, "designated_slots")

    # Buy a real upgrade for the couch slot, then change its physical owner.
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);LobbyStart();', "menu")
    for peer in (host, guest):
        _ready(peer, FIRST)
    _skip(host)
    for peer in (host, guest):
        _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
              'if(c.controlled&&!c.network_proxy)c.static_char=true;}')
    # During a running world bot removal keeps active team assignments. A
    # surviving team's number can exceed the newly reduced unit-slot count.
    gap_removed, gap_survivor = add(), add()
    high_team = len(slots())
    _code(host, f'LobbySetPlayerTeam({gap_survivor},{high_team});LobbyRemovePlayer({gap_removed});')
    wait(lambda: gap_removed not in slots() and slots()[gap_survivor]["team"] == high_team,
         "live bot removal preserves the surviving team")
    live_gap = saved()
    assert high_team > len(live_gap["slots"]) and data(action="validate", saved=live_gap)["valid"]
    _code(host, f'LobbyRemovePlayer({gap_survivor});')
    wait(lambda: gap_survivor not in slots(), "temporary live team fixtures removed")
    hero = host.bridge.me()["id"]
    victim = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                              pos=[0,5,0], teams="slot_xp_enemy")["object_id"]
    _code(host, f'for(int n=0;n<100;++n){{ProgressionReportHit({victim},{hero},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();}ProgressionBeginShop(false);')
    wait(lambda: host.bridge.call("progression", actor=couch)["view"].get("shop_open"), "slot upgrade shop")
    assert host.bridge.call("progression", actor=couch, action="buy", key="bone_health")["submitted"]
    for peer in (host, guest):
        for p in _rows(peer):
            if p["peer_id"] == (255 if peer is host else next(r["peer_id"] for r in _rows(host) if r["id"] == bot)):
                assert peer.bridge.call("progression", actor=p["id"], action="ready")["submitted"]
    _code(host, 'LobbyEndGame();')
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"] for p in (host, guest)),
         "return to slot lobby")

    def rank(slot, peer=host):
        return next(p["bone_health"] for p in peer.bridge.call("progression", actor=slot)["ledger"]["players"] if p["id"] == slot)

    assert rank(couch) == 1
    before = {id: (slot["uid"], slot["order"], slot["team"]) for id, slot in slots().items()}
    action("swap", first=original_host, second=couch)
    assert next(p for p in _rows(host) if p["host"])["id"] == couch
    assert next(p for p in _rows(host) if p["id"] == couch)["local_index"] == 0
    action("swap", first=couch, second=bot)
    wait(lambda: next(p for p in _rows(guest) if p["host"])["id"] == bot,
         "host role follows its physical human through cross-peer slot swap")
    assert host.bridge.status()["local_players"] == guest.bridge.status()["local_players"] == 2
    assert rank(couch) == rank(couch, guest) == 1
    assert {id: (slot["uid"], slot["order"], slot["team"]) for id, slot in slots().items()} == before
    wait(lambda: any(p["id"] == couch and p["peer_id"] != 255 and p["local_index"] == 0 for p in _rows(guest)),
         "remote primary controls the upgraded unit's original slot")
    _code(host, f'LobbyRemovePlayer({remote_couch});', "menu")
    wait(lambda: guest.bridge.status()["local_players"] == 1 and remote_couch not in ids(), "remove remote couch occupant")
    assert remote_couch in slots()
    _code(guest, 'LobbyTryLeave();', "menu")
    wait(lambda: couch not in ids() and not guest.bridge.status()["online"]["active"], "departed human leaves upgraded slot available")
    assert rank(couch) == 1 and couch in slots()
    replacement.bridge.call("connect", ip="127.0.0.1:28671")
    wait(lambda: couch in ids(replacement), "replacement human claims persistent upgraded slot")
    assert rank(couch, replacement) == 1
    extra_bot = add()
    snapshot = saved()
    # Saved host slot is a unit identity, so loading can bind this host to a
    # different saved unit while retaining every connected human/controller.
    snapshot["host_slot"] = couch
    assert data(action="validate", saved=snapshot)["valid"]
    invalid = copy.deepcopy(snapshot)
    invalid["slots"][1]["uid"] = invalid["slots"][0]["uid"]
    before_invalid = saved()
    assert not data(action="validate", saved=invalid)["valid"]
    assert not data(action="restore", saved=invalid)["submitted"]
    assert saved() == before_invalid, "invalid restore mutated slot ownership"
    action("restore", saved=snapshot)
    wait(lambda: next(p for p in _rows(replacement) if p["host"])["id"] == couch,
         "restored saved host unit maps onto the connected host")
    assert rank(couch) == rank(couch, replacement) == 1
    assert extra_bot in ids() and next(p for p in _rows(host) if p["id"] == extra_bot)["bot"]
    assert remote_couch in slots() and remote_couch not in ids()
    assert host.bridge.status()["local_players"] == 2 and replacement.bridge.status()["local_players"] == 1
    # Automatic admission fills free placeholders first, then creates slots.
    action("kind", slot=remote_couch, player=False)
    action("admission", automatic=True)
    before_auto = len(slots())
    guest.bridge.call("connect", ip="127.0.0.1:28671")
    wait(lambda: len(slots()) == before_auto + 1 and len(ids(guest)) == len(ids()), "automatic remote slot creation")
    press_pad(host, 2, 3)
    wait(lambda: len(slots()) == before_auto + 2, "automatic couch slot creation")
    assert len({p["uid"] for p in saved()["slots"]}) == len(slots())
    empty_after_handoff = add(player=True)
    before_handoff = {id: (s["uid"], s["order"], s["kind"]) for id, s in slots().items()}
    replacement_slot = int(menu_probe(replacement, 'LobbyCurrentPlayerID()'))
    _code(host, f'SetConfigValueInt("controller_menu_owner",0);LobbyPromoteHost({replacement_slot});', "menu")
    wait(lambda: replacement.bridge.status()["online"]["hosting"] and
         all(not p.bridge.status()["online"]["migration_busy"] for p in (host, guest, replacement)),
         "network host handoff preserves all unit slots", 90)
    wait(lambda: all({id:(s["uid"],s["order"],s["kind"]) for id,s in slots(p).items()} == before_handoff
                     for p in (host, guest, replacement)), "slot definitions survive transport remapping")
    assert rank(couch, replacement) == rank(couch, guest) == 1
    assert empty_after_handoff not in ids(replacement) and slots(replacement)[empty_after_handoff]["kind"] == "player"
    final = saved(replacement)
    _code(host, 'SetConfigValueInt("controller_menu_owner",0);LobbyTryLeave();', "menu")
    wait(lambda: not host.bridge.status()["online"]["active"], "original host leaves the migrated lobby")
    assert data(host, action="validate", saved=final)["valid"], "Continue preflight must work from an offline menu"
    assert all(not script_errors(p) for p in (host, guest, replacement)), [script_errors(p) for p in (host, guest, replacement)]
    return {"passed": True, "notes": [], "metrics": {"upgraded_slot": couch, "restored_host_slot": final["host_slot"],
            "slot_count": len(final["slots"]), "stable_unit_uid": next(p["uid"] for p in final["slots"] if p["id"] == couch),
            "network_host_handoff": True}}
