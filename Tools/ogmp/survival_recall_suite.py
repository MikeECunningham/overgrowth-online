"""Countdown staging and battery-end physical weapon recovery on a host and guest."""
from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _ready, _rows
from .scenarios import script_errors


def survival_recall(ctx):
    keys = {**RENDER_KEYS, "player_invincible": "true"}
    host = ctx.instance("recall_host", keys={**keys, "mp_port": "29586"})
    guest = ctx.instance("recall_guest", keys={**keys, "mp_port": "29587"})
    peers = (host, guest)

    def wait(fn, label):
        return host.bridge.wait(fn, 40, step=.12, what=label)

    def director(code):
        _code(host, code, "level", "level_specific")

    def items(peer):
        return peer.bridge.call("state")["items"]

    def item(peer, common):
        return next(i for i in items(peer) if i["common_id"] == common)

    def avatar(peer, seat):
        return next(p["avatar_id"] for p in _rows(peer) if p["id"] == seat)

    def held(common, seat):
        return all(item(p, common)["held_by"] == avatar(p, seat) for p in peers)

    def make(path):
        before = {i["common_id"] for i in items(host)}
        director(f'CreateObject("{path}",true);')
        new = next(i["common_id"] for i in items(host) if i["common_id"] not in before and i["file"] == path)
        wait(lambda: all(any(i["common_id"] == new for i in items(p)) for p in peers), "world weapon replicated")
        return new

    def attach(common, actor, socket="_at_grip", mirror="false"):
        director(f'ReadObjectFromID({actor}).AttachItem(ReadObjectFromID({item(host, common)["id"]}),{socket},{mirror});')

    def drop(peer, seat, common):
        local = item(peer, common)["id"]
        _code(peer, f'this_mo.DetachItem({local});NotifyItemDetach({local});ReadItemID({local}).ActivatePhysics();',
              avatar(peer, seat))
        wait(lambda: item(host, common)["held_by"] < 0, "owner dropped the physical item")

    _code(host, 'LobbyBeginHost();')
    guest.bridge.call("connect", ip="127.0.0.1:29586")
    wait(lambda: len(_rows(host)) == 2, "guest joins")
    _code(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false);LobbyAddBot();')
    seats = _rows(host)
    owner = next(p["id"] for p in seats if p["host"])
    visitor = next(p["id"] for p in seats if not p["host"] and not p["bot"])
    bot = next(p["id"] for p in seats if p["bot"])
    for p in seats:
        _code(host, f'LobbySetPlayerTeam({p["id"]},1);')
    _code(host, 'LobbySetModeSetting("survival_seed",8);LobbySetModeSetting("progression",1);'
          'LobbySetModeSetting("progression_shop",1);LobbySetModeSetting("progression_free_buffs",0);'
          'LobbySetModeSetting("progression_timer",0);LobbyStart();')
    for peer in peers:
        _ready(peer, SANDBOX_MAP)

    def raiders():
        return [c for c in host.bridge.state() if "survival_enemies" in c["teams"] and c["enabled"] and not c["knocked_out"]]

    foes = wait(raiders, "first wave deploys")
    for c in foes:
        _code(host, 'this_mo.static_char=true;this_mo.ReceiveScriptMessage("set_combat_allowed false");', c["id"])
    last = foes[0]["id"]
    knife = make("Data/Items/flint_knife_c2.xml")
    staff = make("Data/Items/staffbasic.xml")
    spare = make("Data/Items/rat_weapons/rat_throwing_blade.xml")

    attach(knife, avatar(host, visitor))
    wait(lambda: held(knife, visitor), "guest owns the painted knife")
    drop(guest, visitor, knife)
    attach(knife, last, "_at_sheathe")
    wait(lambda: item(host, knife)["held_by"] == last, "last enemy took and sheathed the guest's knife")

    attach(staff, avatar(host, owner))
    wait(lambda: held(staff, owner), "host owns the staff")
    drop(host, owner, staff)
    attach(staff, avatar(host, bot))
    wait(lambda: held(staff, bot), "companion carries the host's staff")
    attach(spare, avatar(host, owner))
    wait(lambda: held(spare, owner), "host owns the spare blade")
    drop(host, owner, spare)
    for c in raiders():
        _code(host, 'SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);', c["id"])
    wait(lambda: _probe(host, "int(phase)", "level", "level_specific") == "4", "finishing camera")
    assert item(host, spare)["held_by"] < 0
    wait(lambda: host.bridge.call("progression", actor=owner)["view"]["shop_open"], "shop opens")
    wait(lambda: held(knife, visitor) and held(spare, owner), "host and remote guest recover original physical items")
    assert held(staff, bot), "a carried weapon was stolen from a companion"

    # A recovered wave weapon must stay where it was dropped when all four
    # compatible sockets are occupied, including during the next battery cleanup.
    loose = make("Data/Items/flint_knife.xml")
    actor = avatar(host, owner)
    attach(loose, actor)
    wait(lambda: held(loose, owner), "host picks up a wave weapon")
    drop(host, owner, loose)
    director(f'ReadObjectFromID({item(host, loose)["id"]}).GetScriptParams().SetInt("Survival Weapon",1);')
    for i, (socket, mirror) in enumerate((("_at_grip", "true"), ("_at_grip", "false"),
                                          ("_at_sheathe", "false"), ("_at_sheathe", "true"))):
        if int(_probe(host, f'weapon_slots[{i}]', actor)) < 0:
            blocker = make("Data/Items/flint_knife.xml")
            attach(blocker, actor, socket, mirror)
            wait(lambda: held(blocker, owner), "compatible inventory slot filled")
    director("Online_RecallSurvivalWeapons();")
    assert item(host, loose)["held_by"] < 0 and item(guest, loose)["held_by"] < 0

    def prepared(peer):
        return {c["common_id"] for c in peer.bridge.state()
                if not c["enabled"] and float(c["pos"][1]) > -98 and
                _probe(peer, f'ReadObjectFromID({c["id"]}).GetScriptParams().HasParam("Survival Staged")', "level") == "true"}
    staged = wait(lambda: prepared(host) & prepared(guest), "next battery has dormant bodies on both peers")
    assert not raiders()
    for seat in [owner, visitor]:
        peer = host if seat == owner else guest
        peer.bridge.call("progression", actor=seat, action="ready")
    director("due=the_time;")
    wait(lambda: _probe(host, "int(phase)", "level", "level_specific") == "0", "first wave countdown")
    director("due=the_time;")
    wait(lambda: bool(staged & {r["common_id"] for r in raiders()}), "prepared bodies activate at the timer")
    assert item(host, loose)["held_by"] < 0 and item(guest, loose)["held_by"] < 0
    assert not script_errors(host) and not script_errors(guest)
    return {"passed": True, "metrics": {"original_weapons_returned": 2, "companion_kept_staff": True,
                                         "loose_weapon_survived_full_inventory": True,
                                         "prepared_ids_reused": len(staged & {r["common_id"] for r in raiders()})}, "notes": []}
