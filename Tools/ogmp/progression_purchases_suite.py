"""Career purchases in a live campaign with two allies and an opposing human."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _probe, _ready, _rows, _skip
from .scenarios import script_errors


def progression_purchases(ctx):
    game = ctx.instance("xp_purchase_curve", width=800, height=450, keys={
        **RENDER_KEYS, "mp_port": "28495", "player_invincible": "true", "difficulty_scaling": "false",
        "armor_localized": "2", "tutorials": "false"})
    bridge = game.bridge
    _code(game, "LobbyBeginHost(); LobbyAddLocalPlayer(); LobbyAddLocalPlayer();", "menu")
    p1, p2, rival = [p["id"] for p in sorted(_rows(game), key=lambda p: p["local_index"])]
    _code(game, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          f'LobbySetForceTeams(true);LobbySetPlayerTeam({rival},2);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);LobbyStart();', "menu")
    _ready(game, FIRST)
    _skip(game)

    def avatar(id):
        return next(p["avatar_id"] for p in _rows(game) if p["id"] == id)

    def details(id=p1, target=None, **kw):
        return bridge.call("progression", actor=id, target=id if target is None else target, **kw)

    def view(id=p1, target=None):
        return details(id, target)["view"]

    def cards(id=p1, target=None):
        return {c["id"]: c for c in view(id, target)["cards"]}

    def act(id, action, key="", target=None):
        assert details(id, target, action=action, key=key)["submitted"], (id, action, key, target)

    def buy(id, key, target=None):
        before = view(id)["held"]
        cost = cards(id, target)[key]["cost"]
        act(id, "buy", key, target)
        assert view(id)["held"] == before - cost, (key, cost)
        return cost

    def wait(fn, label, seconds=20):
        bridge.wait(fn, seconds, step=.1, what=label)

    def shop():
        _code(game, "ProgressionBeginShop(false);")
        wait(lambda: view().get("shop_open"), "between-fight shop opens")

    def leave():
        for id in (p1, p2, rival):
            act(id, "ready")
        wait(lambda: not view().get("shop_open"), "all three players close shop")

    def build(id):
        return next(p for p in details()["ledger"]["players"] if p["id"] == id)

    def equipment(id):
        state = bridge.call("state")
        actor = next(c for c in state["characters"] if c["id"] == avatar(id))
        return {item["file"].split("/")[-1].lower() for item in state["items"] if item["id"] in actor["weapon_slots"][:4]}

    def armor_files(id):
        actor = next(c for c in bridge.state() if c["id"] == avatar(id))
        return {p["path"].split("/")[-1] for p in actor["armor_attachments"]}

    def rating(id):
        return float(_probe(game, f"Online_WornArmorRating({avatar(id)})"))

    def portrait(id, label):
        def value(expression):
            bridge.call("level_messages",prefix="armor_portrait")
            _code(game,'level.SendMessage("armor_portrait "+('+expression+'));',avatar(id))
            messages = bridge.call("level_messages",prefix="armor_portrait")["messages"]
            assert messages,expression
            return float(messages[-1].split(" ",1)[1])
        facing = [value(f'this_mo.GetFacing().{axis}') for axis in ("x","y","z")]
        target = [value(f'this_mo.rigged_object().GetAvgIKChainPos("torso").{axis}') for axis in ("x","y","z")]
        for view, direction in (("front",facing),("side",[-facing[2],0,facing[0]])):
            bridge.call("view",pos=[target[i]+direction[i]*2+(0.12 if i == 1 else 0) for i in range(3)],
                        target=target,fov=43,pause=True)
            ctx.shot(game,label+"_"+view)
            bridge.call("view",clear=True)

    # Compress many campaigns into a bounded fixture. Every defeat still
    # runs the normal victim/attacker eligibility and checkpoint bank.
    hero = avatar(p1)
    enemy = bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                        pos=[0, 5, 0], teams="xp_training_enemy")["object_id"]
    def earn(count):
        _code(game, f'for(int n=0;n<{count};++n){{ProgressionReportHit({enemy},{hero},0,true);'
              'ProgressionCheckpoint();ProgressionResetAttempt();}')

    for _ in range(30):
        if view()["held"] >= 25000:
            break
        earn(100)
    assert view()["held"] >= 25000 and view(p2)["held"] >= 5400
    shop()
    assert buy(p1, "recruit") == 2400
    first = next(p["id"] for p in _rows(game) if p["bot"])
    assert build(first)["trained"] and build(first)["ai"] == 0 and build(first)["owner"] == p1
    assert cards(rival)["recruit"]["cost"] == 2400
    assert buy(p2, "recruit") == 5400
    second = next(p["id"] for p in _rows(game) if p["bot"] and p["id"] != first)
    assert build(second)["owner"] == p2 and cards(p1)["recruit"]["cost"] == 12150
    assert cards(rival)["recruit"]["cost"] == 2400
    assert not ({"bone_health", "speed", "damage", "flesh_health", "fall_resistance"} & cards(p1, first).keys())
    ai_prices = []
    for tier, cost in enumerate((1800, 3400, 8600), start=1):
        ai_prices.append(buy(p1, "ai", first))
        assert ai_prices[-1] == cost and build(first)["ai"] == tier
    assert "ai" not in cards(p1, first)
    available = cards(p1, first)
    assert available["armor_0"]["label"] == "Chitin / Chest" and available["armor_0"]["cost"] == 600
    assert available["armor_3"]["label"] == "Wood / Left forearm" and available["armor_3"]["armor_material"] == 1
    assert buy(p1, "armor_3", first) == 80
    for region, price in enumerate((80, 80, 125, 125, 100, 100), 3):
        assert buy(p2, f"armor_{region}") == price
    assert buy(p1, "dagger") == 350
    assert buy(p1, "sword") == 1200 and buy(p1, "spear") == 1000
    assert view()["owned_sword"] and view()["owned_spear"] and view()["owned_dagger"]
    for key, choice in (("none", 0), ("spear", 2), ("sword", 1)):
        act(p1, "equip", key)
        assert view()["big_weapon"] == choice and view()["dagger"]
    act(p1, "equip", "dagger")
    assert not view()["dagger"] and view()["big_weapon"] == 1
    act(p1, "equip", "dagger")
    leave()
    wait(lambda: avatar(first) >= 0 and avatar(second) >= 0, "recruits have live avatars")
    wait(lambda: {"dogsword.xml", "rabbit_knife.xml"} <= equipment(p1), "sword and offhand dagger both issued")
    assert "dogspear.xml" not in equipment(p1)
    def reaches(tier, vitals=0):
        expected = .065 * (0, .5, .85, 1.3, 1.75)[tier] + vitals
        wait(lambda: math.isclose(rating(first), expected, abs_tol=.001), f"equipped weighted protection {expected}")
        return rating(first)
    ratings = [reaches(1)]
    assert "large_wood_plate.xml" in armor_files(first)

    def wooden_guards_fit():
        actor = next(c for c in bridge.state() if c["id"] == avatar(p2))
        guards = [p for p in actor["armor_attachments"] if p["path"].endswith("large_wood_plate.xml")]
        return len(guards) == 6 and all(p["fit_valid"] and p["fit_error"] < .15 and
                                        math.dist(actor["pos"], p["pos"]) < 2 for p in guards) and all(
            p["long_axis_alignment"] > .8 for p in guards if p["region"] in (8,16))
    wait(wooden_guards_fit, "both arm, shoulder and thigh guards follow their stock sockets")
    wood_alignments = [p["long_axis_alignment"] for c in bridge.state() if c["id"] == avatar(p2)
                       for p in c["armor_attachments"] if p["path"].endswith("large_wood_plate.xml") and p["region"] in (8,16)]
    assert math.isclose(rating(p2), 2*(.065+.09+.095)*.5, abs_tol=.001)
    portrait(p2,"wood_guard_alignment")
    # Only the trained ally earns XP. Confirm its stats rise at the next
    # shop, with at most one rank difference, alongside normal player XP.
    earn(15)
    assert build(first)["bone_health"] == build(first)["damage"] == build(first)["flesh_health"] == build(first)["speed"] == 0
    shop()
    ranks = [build(first)[k] for k in ("bone_health", "damage", "flesh_health", "speed", "fall_resistance")]
    assert min(ranks) > 0 and max(ranks) - min(ranks) <= 1, ranks
    assert all(build(second)[k] == 0 for k in ("bone_health", "damage", "flesh_health", "speed", "fall_resistance"))
    assert cards(p1, first)["armor_3"]["label"] == "Chitin / Left forearm"
    assert buy(p1, "armor_3", first) == 160
    act(p1, "equip", "spear")
    leave()
    ratings.append(reaches(2))
    assert "large_wood_plate.xml" not in armor_files(first) and "armchunk.xml" in armor_files(first)
    portrait(first,"chitin_guard")
    shop()
    assert buy(p1, "armor_3", first) == 240
    leave()
    ratings.append(reaches(3))
    assert "armchunk.xml" not in armor_files(first) and "dog_armor_lower_arm.xml" in armor_files(first)
    # Normal checkpoint retries rebuild the chosen loadout on this level.
    _code(game, 'level.SendMessage("reset");')
    _skip(game)
    wait(lambda: {"dogspear.xml", "rabbit_knife.xml"} <= equipment(p1), "spear and offhand dagger issued on retry")
    assert "dogsword.xml" not in equipment(p1)
    shop()
    assert cards(p1, first)["armor_3"]["label"] == "Therium grey / Left forearm"
    assert buy(p1, "armor_3", first) == 400 and "armor_3" not in cards(p1, first)
    assert buy(p1, "armor_0", first) == 600
    for slot in range(9):
        assert cards(p2)[f"armor_{slot}"]["armor_material"] == 2
        buy(p2,f"armor_{slot}")
    leave()
    ratings.append(reaches(4, .28 * .85))
    assert any("dog_armour_grey/" in p["path"] for c in bridge.state() if c["id"] == avatar(first) for p in c["armor_attachments"])
    def full_chitin_fits():
        actor = next(c for c in bridge.state() if c["id"] == avatar(p2))
        pieces = [p for p in actor["armor_attachments"] if p["material"] == 2]
        return len(pieces) == 9 and {p["region"] for p in pieces} == {1<<i for i in range(9)} and all(
            "/chitinarmour/" in p["path"] and p["fit_valid"] and p["fit_error"] < .15 and
            math.dist(actor["pos"],p["pos"]) < 2.5 for p in pieces)
    wait(full_chitin_fits,"all nine chitin pieces follow their intended body slots")
    portrait(p2,"chitin_full_suit")
    assert not script_errors(game), script_errors(game)
    return {"passed": True, "metrics": {"recruit_prices": [2400, 5400, 12150], "ai_prices": ai_prices,
                                        "nightmare_ranks": ranks, "armor_ratings": ratings,
                                        "wood_forearm_alignment": wood_alignments}, "notes": []}
