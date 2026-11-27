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
    def panel():
        return next(v for v in host.bridge.status()["progression_ui"] if v["actor"] == owner)
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
    def visible_shop():
        layout = panel()
        return layout if layout.get("sections") and layout.get("action_rects") else None
    layout = wait(visible_shop, "leader can see grouped upgrades and shop hit targets")
    assert layout["sections"] == ["Stats", "Armor", "Daggers", "Main-hand weapons", "Characters"]
    for box in layout["action_rects"].values():
        assert box[3] - box[1] >= 30
    hero = fighter(host, owner)["id"]
    director('int blade=CreateObject("Data/Items/rat_weapons/rat_throwing_blade.xml",true);'
             f'ReadObjectFromID({hero}).AttachItem(ReadObjectFromID(blade),_at_grip,false);')
    def carried_blade():
        return next((row["item"] for row in view()["inventory"] if row.get("label") == "Rat throwing blade"), None)
    blade = wait(carried_blade, "mouse holds an unpurchased throwing blade")
    def owned_races(peer, who, target):
        v = peer.bridge.call("progression", actor=who, target=target)["view"]
        return [r["id"] for r in v["races"]], v["equipped_race"]

    def wear(peer, who, target, species):
        return peer.bridge.call("progression", actor=who, target=target, action="equip_race", key=species)["submitted"]

    assert owned_races(host, owner, owner) == (["rat"], "rat")
    assert owned_races(host, owner, bot) == (["rabbit"], "rabbit")
    assert not wear(host, owner, owner, "cat") and not wear(host, owner, bot, "rat")
    for _ in range(2):
        buy("race_upgrade", owner, 1200)
    expect_races({owner: "/turner.xml", visitor: "/turner.xml"})
    assert wear(host, owner, owner, "rat")
    assert not wear(host, owner, visitor, "rat")
    expect_races({owner: "/rat.xml", visitor: "/turner.xml"})
    # The Mouse-wearing owner already bought Rabbit. The group's next two
    # purchases therefore rotate Cat at 2,400 XP each.
    for _ in range(2):
        buy("race_upgrade", owner, 2400)
    assert owned_races(host, owner, owner) == (["rat", "rabbit", "cat"], "cat")
    for price in (2400, 9600):
        buy("race_upgrade", bot, price)
    assert wear(host, owner, bot, "rabbit")
    buy("race_upgrade", bot, 38400)
    upgraded = {owner: "/male_cat.xml", visitor: "/male_cat.xml", bot: "/male_wolf.xml"}
    expect_races(upgraded)
    assert owned_races(host, owner, bot) == (["rabbit", "cat", "dog", "wolf"], "wolf")
    assert carried_blade() == blade, "changing species lost the carried weapon"
    assert not any(c["id"] == "race_upgrade" for c in view(bot)["cards"])
    for seat in (owner, bot):
        _code(host, "RecoverHealth();", fighter(host, seat)["id"])
    _code(guest, "permanent_health=.2f;temp_health=.2f;blood_health=.6f;block_health=1.0f;", fighter(guest, visitor)["id"])
    wait(lambda: card("restore_health")["heal_quote"] == 8, "host sees the guest's bone injury")
    heal = card("restore_health")
    assert heal["cost"] == 2.5 and heal["full_cost"] == 10 and heal["heal_fraction"] == 2.5 / 8
    host.bridge.call("key", key="Space")
    wait(lambda: all(math.isclose(fighter(p, visitor)["permanent_health"], .45, abs_tol=.005)
                     for p in peers), "only the purchased fraction heals on both peers")
    assert not card("restore_health")["available"]
    host.bridge.call("key", key="Return")
    wait(lambda: view()["ready"], "Enter readies the keyboard seat after healing")
    assert not guest.bridge.call("progression", actor=visitor)["view"]["ready"]
    host.bridge.call("key", key="Keypad Enter")
    wait(lambda: not view()["ready"], "keypad Enter toggles only this seat back")
    assert wear(guest, visitor, visitor, "rat") and wear(host, owner, bot, "cat")
    chosen = {owner: "/male_cat.xml", visitor: "/rat.xml", bot: "/male_cat.xml"}
    expect_races(chosen)
    save(2)
    load(2)
    expect_races(chosen)
    assert owned_races(host, owner, bot) == (["rabbit", "cat", "dog", "wolf"], "cat")
    assert owned_races(guest, visitor, visitor) == (["rat", "rabbit", "cat"], "rat")
    assert not any(c["id"] == "race_upgrade" for c in view(bot)["cards"])
    assert view()["held"] == 0
    # The nominal shopping time elapsed; the store is still usable during the base break.
    director("due=the_time+1;")
    assert view()["shop_open"]
    wait(lambda: state("int(phase)") == "0", "next battery deploys")
    wait(lambda: state("int(phase)") == "1" and not view()["shop_open"], "unready shop closes when fighting resumes")
    assert not script_errors(host) and not script_errors(guest), (script_errors(host), script_errors(guest))
    return {"passed": True, "metrics": {"partial_heal_xp": 2.5, "companion_highest": "wolf", "companion_equipped": "cat", "shop_seconds": 10}, "notes": []}


def campaign_race_equipment(ctx):
    """A legacy race upgrade and a new equipment choice on an authored cat protagonist."""
    import time

    campaign = "com-wolfire-drika"
    level = "drika/foltisearch.xml"
    host = ctx.instance("race_campaign_host", width=1280, height=720, keys={
        **RENDER_KEYS, "mp_port": "29901", "player_invincible": "true"})
    bridge = host.bridge

    def wait(predicate, label):
        return bridge.wait(predicate, 50, step=.1, what=label)

    def account():
        return bridge.call("progression", actor=seat)["view"]

    def form():
        return next((row for row in bridge.status()["progression_ui"] if row["actor"] == seat), {})

    def avatar():
        body = _rows(host)[0]["avatar_id"]
        return next((actor for actor in bridge.state() if actor["id"] == body), {})

    def save(slot):
        _code(host, f"SessionSave({slot});", "level")
        path = Path(host.writedir, f"Data/SessionSaves/slot{slot}.json")
        wait(path.is_file, "open shop saved")
        return path

    def load(slot):
        _code(host, f"SessionLoad({slot});", "level")
        wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"],
             "card opens lobby")
        _code(host, "LobbyStart();")
        _ready(host, level)
        wait(lambda: account().get("shop_open"), "saved shop restored")

    def click_race(species):
        def rect():
            return form().get("race_rects", {}).get("race_"+species)
        xy = wait(rect, "owned race button visible in Equipment")
        pos = dict(x=round((xy[0]+xy[2])/2), y=round((xy[1]+xy[3])/2))
        for buttons in (0, 1, 0):
            bridge.call("pointer", **pos, buttons=buttons)
            time.sleep(.09)

    _code(host, "LobbyBeginHost();")
    seat = _rows(host)[0]["id"]
    _code(host, f'LobbySetSelection("{campaign}","{campaign}","Data/Levels/{level}",false);'
          'LobbySetModeSetting("progression",1);LobbyStart();')
    _ready(host, level)
    wait(lambda: avatar().get("char_path","").endswith("fancy_striped_cat.xml"), "preserved campaign protagonist")
    _code(host, "ProgressionBeginShop(false);", "level")
    wait(lambda: form().get("columns"), "both shop columns drawn")
    assert account()["races"] == [{"id":"cat", "name":"Cat"}] and account()["equipped_race"] == "cat"
    assert not bridge.call("progression", actor=seat, action="equip_race", key="rabbit")["submitted"]
    # Old savecards stored only the most recently purchased race.
    path = save(1)
    record = json.loads(path.read_text())
    runtime = record["session_mods"]["og-online-progression"]["runtime"]
    row = next(p for p in runtime["players"] if p["id"] == seat)
    row["race"] = 3
    assert "first_race" not in row and "equipped_race" not in row
    path.write_text(json.dumps(record))
    load(1)
    wait(lambda: avatar().get("char_path","").endswith("dog_soldier.xml"), "legacy purchased dog deployed")
    wait(lambda: [r["id"] for r in account()["races"]] == ["cat","dog"], "campaign actor recovers starting race")
    before = (account()["held"], account()["pooled"])
    click_race("cat")
    wait(lambda: avatar().get("char_path","").endswith("fancy_striped_cat.xml"), "original striped cat re-equipped")
    assert account()["equipped_race"] == "cat"
    assert (account()["held"], account()["pooled"]) == before
    card = next(c for c in account()["cards"] if c["id"]=="race_upgrade")
    assert card["label"] == "Dog to Wolf" and card["cost"] == 38400
    saved = json.loads(save(2).read_text())["session_mods"]["og-online-progression"]["runtime"]
    row = next(p for p in saved["players"] if p["id"]==seat)
    assert (row["first_race"], row["race"], row["equipped_race"]) == (2, 3, 2)
    load(2)
    wait(lambda: avatar().get("char_path","").endswith("fancy_striped_cat.xml"), "selected starting cat survives reload")
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"starting_race":"cat","legacy_owned":"dog","selected":"cat",
                                        "next_upgrade_xp":card["cost"]}, "notes":[]}
