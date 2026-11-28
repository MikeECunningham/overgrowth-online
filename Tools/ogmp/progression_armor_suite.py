"""Shared armor upgrades distribute each slot tier before advancing materials."""
from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _rows, _skip
from .scenarios import script_errors


def progression_shared_armor(ctx):
    game = ctx.instance("shared_armor_tiers", width=960, height=540, keys={
        **RENDER_KEYS, "mp_port": "28595", "player_invincible": "true",
        "difficulty_scaling": "false", "armor_localized": "2", "tutorials": "false"})
    bridge = game.bridge
    _code(game, 'LobbyBeginHost();LobbyAddLocalPlayer();LobbyAddBot();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",1);'
          'LobbySetModeSetting("progression_timer",0);LobbyStart();', "menu")
    _ready(game, FIRST)
    _skip(game)
    members = sorted(p["id"] for p in _rows(game))
    bot = next(p["id"] for p in _rows(game) if p["bot"])
    buyer = next(p["id"] for p in _rows(game) if p["host"])

    def data(target=buyer, **kw):
        return bridge.call("progression", actor=buyer, target=target, **kw)

    def cards(target=buyer):
        return {c["id"]: c for c in data(target)["view"]["cards"]}

    def builds():
        return {p["id"]: p["armor"] for p in data()["ledger"]["players"]}

    hero = next(p["avatar_id"] for p in _rows(game) if p["id"] == buyer)
    enemy = bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                        pos=[0,5,0], teams="armor_reward_enemy")["object_id"]
    _code(game, f'for(int n=0;n<2000;++n){{ProgressionReportHit({enemy},{hero},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();}ProgressionBeginShop(false);')
    bridge.wait(lambda: data()["view"].get("shop_open"), 15, what="shared armor shop")
    assert data()["view"]["held"] > 12000
    assert cards()["armor_0"]["armor_material"] == 2  # No invented wood vital mesh.

    recipients = []
    def buy(slot, tier, target=buyer):
        key = f"armor_{slot}"
        card = cards(target)[key]
        assert card["armor_material"] == card["rarity"] == tier, card
        before = builds()
        response = data(target, action="buy", key=key)
        assert response["submitted"]
        after = builds()
        changed = [id for id in members if before.get(id,[0]*9)[slot] != after[id][slot]]
        assert len(changed) == 1, (before,after)
        recipient = changed[0]
        assert after[recipient][slot] == tier
        recipients.append(recipient)
        return recipient

    assert buy(3,1) == members[0]
    assert buy(4,1) == members[1]  # Rotation spans cards, rather than restarting per slot.
    assert buy(3,1,bot) == members[2]  # Selecting a bot cannot bypass shared distribution.
    assert cards()["armor_3"]["armor_material"] == 1
    assert buy(3,1) == members[1]  # Skip the recipients who already own this tier.
    for tier in (2,3,4):
        for n in range(len(members)):
            assert cards()["armor_3"]["armor_material"] == tier
            buy(3,tier,bot if n == 1 else buyer)
        assert all(builds()[id][3] == tier for id in members)
    assert "armor_3" not in cards()
    assert cards()["armor_4"]["armor_material"] == 1  # Other slots retain their own gate.
    # The first vital tier must also reach everyone before Dog armor is offered.
    for _ in members:
        buy(0,2)
    assert cards()["armor_0"]["armor_material"] == 3
    ctx.shot(game,"all_material_cards")
    assert bridge.call("progression",actor=buyer,action="ready")["submitted"]
    bridge.wait(lambda: not data()["view"].get("shop_open"),15,what="shared armor issued")
    def issued():
        avatars = {p["avatar_id"] for p in _rows(game)}
        actors = [c for c in bridge.state() if c["id"] in avatars]
        return len(actors) == len(members) and all(any(p["material"] == 4 and p["region"] == 8 and
               "dog_armour_grey/" in p["path"] for p in c["armor_attachments"]) for c in actors)
    bridge.wait(issued,20,what="every shared recipient wears real grey armor")
    assert not script_errors(game),script_errors(game)
    return {"passed":True,"metrics":{"members":members,"recipients":recipients},"notes":[]}
