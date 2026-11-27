"""Live Survival race progression, a remote partial heal and timed shop deployment."""
import json
import math
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _ready, _rows
from .scenarios import script_errors


def survival_shop(ctx):
    host = ctx.instance("survival_shop_host", keys={**RENDER_KEYS, "mp_port": "29861"})
    guest = ctx.instance("survival_shop_guest", keys={**RENDER_KEYS, "mp_port": "29862"})
    peers = (host, guest)

    def wait(fn, what):
        return host.bridge.wait(fn, 45, step=.12, what=what)

    def director(code):
        _code(host, code, "level", "level_specific")

    def state(expr):
        return _probe(host, expr, "level", "level_specific")

    def fighter(peer, seat):
        avatar = next(p["avatar_id"] for p in _rows(peer) if p["id"] == seat)
        return next((c for c in peer.bridge.state() if c["id"] == avatar), {})

    def races(peer):
        characters = {row["id"]: row["char_path"] for row in peer.bridge.state()}
        return {p["id"]: characters.get(p["avatar_id"], "") for p in _rows(peer)}

    def expect_races(expected):
        def matched():
            return all(all(row.get(seat, "").endswith(filename) for seat, filename in expected.items())
                       for row in map(races, peers))
        try:
            wait(matched, "roster races on host and guest")
        except Exception as exc:
            raise AssertionError({"expected": expected, "host": races(host), "guest": races(guest)}) from exc

    def view(target=None):
        return host.bridge.call("progression", actor=owner, target=owner if target is None else target)["view"]

    def card(key, target=None):
        return next(c for c in view(target)["cards"] if c["id"] == key)

    def buy(key, target, price):
        before = view()["held"]
        assert card(key, target)["cost"] == price, card(key, target)
        assert host.bridge.call("progression", actor=owner, target=target, action="buy", key=key)["submitted"]
        wait(lambda: math.isclose(view()["held"], before - price, abs_tol=.0001), "purchase settled")

    def save(slot):
        path = Path(host.writedir, f"Data/SessionSaves/slot{slot}.json")
        director(f"SessionSave({slot});")
        wait(path.exists, "save written")
        return path

    def load(slot):
        director(f"SessionLoad({slot});")
        wait(lambda: host.bridge.status()["online"]["lobby_waiting"] and not host.bridge.status()["level_loaded"], "save lobby")
        _code(host, "LobbyStart();")
        for peer in peers:
            _ready(peer, SANDBOX_MAP)
        wait(lambda: view().get("shop_open"), "restored open shop")

    _code(host, "LobbyBeginHost();")
    guest.bridge.call("connect", ip="127.0.0.1:29861")
    wait(lambda: len(_rows(host)) == 2, "guest joins")
    _code(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false);LobbyAddBot();')
    seats = _rows(host)
    owner = next(p["id"] for p in seats if p["host"])
    visitor = next(p["id"] for p in seats if not p["host"] and not p["bot"])
    bot = next(p["id"] for p in seats if p["bot"])
    for seat in seats:
        _code(host, f'LobbySetPlayerTeam({seat["id"]},1);')
    for key, value in {"survival_starting_race": 0, "progression": 1, "progression_shared": 1,
                       "progression_shop": 1, "progression_free_buffs": 0, "progression_timer": 1}.items():
        assert _probe(host, f'LobbySetModeSetting("{key}",{value})') == "true"
    _code(host, "LobbyStart();")
    for peer in peers:
        _ready(peer, SANDBOX_MAP)
    starting = {owner: "/rat.xml", visitor: "/rat.xml"}
    expect_races(starting)
    wait(lambda: fighter(host, bot) and _probe(host, "species", fighter(host, bot)["id"]) == "0",
         "companion starts as a rabbit")

    def opponents():
        return [c for c in host.bridge.state() if "survival_enemies" in c["teams"] and c["knocked_out"] == 0]
    wait(opponents, "first wave spawns")
    for actor in opponents():
        _code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", actor["id"])
    wait(lambda: view().get("shop_open"), "battery shop opens")
    remaining = float(state("due - the_time"))
    assert 18 < remaining <= 22.01, remaining  # stock 12-second break plus ten seconds for shopping
    path = save(1)
    record = json.loads(path.read_text())
    ledger = record["session_mods"]["og-online-progression"]["runtime"]
    team = next(t for t in ledger["teams"] if t["team"] == 1)
    team["leader"] = owner
    team["w"]["held"] = 57602.5
    path.write_text(json.dumps(record))
    load(1)
    director("due = the_time + 120;")
    expect_races(starting)
    hero = fighter(host, owner)["id"]
    director('int blade=CreateObject("Data/Items/rat_weapons/rat_throwing_blade.xml",true);'
             f'ReadObjectFromID({hero}).AttachItem(ReadObjectFromID(blade),_at_grip,false);')
    def carried_blade():
        return next((row["item"] for row in view()["inventory"] if row.get("label") == "Rat throwing blade"), None)
    blade = wait(carried_blade, "mouse holds an unpurchased throwing blade")
    for price in (1200, 1200, 2400, 2400):
        buy("race_upgrade", owner, price)
    for price in (2400, 9600, 38400):
        buy("race_upgrade", bot, price)
    upgraded = {owner: "/male_cat.xml", visitor: "/male_cat.xml", bot: "/male_wolf.xml"}
    expect_races(upgraded)
    assert carried_blade() == blade, "changing species lost the carried weapon"
    assert not any(c["id"] == "race_upgrade" for c in view(bot)["cards"])
    for seat in (owner, bot):
        _code(host, "RecoverHealth();", fighter(host, seat)["id"])
    _code(guest, "permanent_health=.2f;temp_health=.2f;blood_health=.6f;block_health=1.0f;", fighter(guest, visitor)["id"])
    wait(lambda: card("restore_health")["heal_quote"] == 8, "host sees the guest's bone injury")
    heal = card("restore_health")
    assert heal["cost"] == 2.5 and heal["full_cost"] == 10 and heal["heal_fraction"] == 2.5 / 8
    buy("restore_health", owner, 2.5)
    wait(lambda: all(math.isclose(fighter(p, visitor)["permanent_health"], .45, abs_tol=.005)
                     for p in peers), "only the purchased fraction heals on both peers")
    assert not card("restore_health")["available"]
    save(2)
    load(2)
    expect_races(upgraded)
    assert view()["held"] == 0
    # The nominal shopping time elapsed; the store is still usable during the base break.
    director("due=the_time+1;")
    assert view()["shop_open"]
    wait(lambda: state("int(phase)") == "0", "next battery deploys")
    wait(lambda: state("int(phase)") == "1" and not view()["shop_open"], "unready shop closes when fighting resumes")
    assert not script_errors(host) and not script_errors(guest), (script_errors(host), script_errors(guest))
    return {"passed": True, "metrics": {"partial_heal_xp": 2.5, "companion_final_race": "wolf", "shop_seconds": 10}, "notes": []}
