"""Buy armor and a loadout, then take them through a chapter change and a campaign loop."""
from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, SECOND, _code, _probe, _ready, _rows, _skip
from .scenarios import script_errors


def progression_campaign(ctx):
    game = ctx.instance("xp_campaign_loop", width=800, height=450, keys={
        **RENDER_KEYS, "mp_port": "28493", "player_invincible": "true", "difficulty_scaling": "true",
        "armor_localized": "2", "tutorials": "false"})
    bridge = game.bridge
    _code(game, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("challenge_round",2);LobbyStart();', 'menu')
    _ready(game, FIRST)
    _skip(game)
    player = _rows(game)[0]["id"]

    def view():
        return bridge.call("progression", actor=player)["view"]

    def act(action, key=""):
        assert bridge.call("progression", actor=player, action=action, key=key)["submitted"], (action, key, view())

    def wait(fn, label):
        return bridge.wait(fn, timeout=45, what=label)

    def worn():
        return float(_probe(game, 'Online_WornArmorRating(' + str(bridge.me()["id"]) + ')'))

    def has_knife():
        return any(item >= 0 for item in bridge.me()["weapon_slots"][:4])

    def win():
        _code(game, 'SendGlobalMessage("levelwin");')
        wait(lambda: view().get("shop_open"), "authored chapter victory opens shop")

    # Compress several chapters of earning into the fixture, retaining real
    # eligibility, price checks, checkpoint banking and shop transactions.
    for _ in range(12):
        _code(game, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ e=ReadCharacter(i);'
              'if(!e.OnSameTeam(ReadCharacterID(0)))ProgressionReportHit(e.GetID(),0,0,true);}'
              'ProgressionCheckpoint();ProgressionResetAttempt();')
    win()
    held = view()["held"]
    for key in ("bone_health","armor_3","dagger"):
        act("buy", key)
    remaining = view()["held"]
    assert remaining == held - (16+155+110), (held, remaining)
    act("ready")
    _ready(game, SECOND)
    _skip(game)
    try:
        wait(lambda: worn() >= .0552 and has_knife(), "purchased wood and weapon spawn in next chapter")
    except Exception:
        actor = bridge.me()
        print("gear diagnostic", actor["id"], actor["weapon_slots"], actor["armor_attachments"],
              bridge.call("progression", actor=player), worn(), flush=True)
        raise
    avatar = bridge.me()["id"]
    assert int(_probe(game, f'ReadCharacterID({avatar}).GetIntVar("progression_health")')) == 1
    assert view()["held"] == remaining
    # Jump over the intervening chapters; the last win and loop still take the
    # exact same campaign and ready/continue paths used during ordinary play.
    _code(game, 'LoadLevelID("sky_ark");')
    _ready(game, "27_Sky_Ark.xml")
    win()
    assert view()["can_loop"] and view()["final"]
    act("loop", "yes")
    act("ready")
    _ready(game, FIRST)
    _skip(game)
    wait(lambda: has_knife() and worn() >= .0552, "equipped loadout and armor survive loop")
    assert view()["held"] == remaining
    challenge = bridge.status()["online"]["challenge"]
    assert challenge["loop"] == 1 and challenge["pressure"] > 0, challenge
    assert not script_errors(game), script_errors(game)
    return {"passed": True, "metrics": {"remaining_xp": remaining, "armor_rating": worn(),
                                          "loop": challenge["loop"]}, "notes": []}
