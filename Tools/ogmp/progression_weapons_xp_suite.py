"""Stock staff/broadsword, player stats, reward-scaled bars and Arena shop timing."""
import json
import math
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _rows, _skip
from .scenarios import script_errors


def progression_weapons_xp(ctx):
    host = ctx.instance("progression_weapons_host", width=1280, height=720, keys={
        **RENDER_KEYS, "mp_port": "28581", "difficulty_scaling": "true", "game_difficulty": "0.5",
        "campaign_mp_scaling": "false", "campaign_armor_scaling": "false",
        "campaign_weapon_scaling": "false", "campaign_ai_scaling": "false",
        "player_invincible": "true", "armor_localized": "2", "tutorials": "false",
        "oo_arena_bots": "0", "oo_arena_random": "false",
    })
    guest = ctx.instance("progression_weapons_guest", width=800, height=600,
                         keys={**RENDER_KEYS, "mp_port": "28582"})
    peers = host, guest

    def wait(predicate, label, seconds=40):
        try:
            return host.bridge.wait(predicate, seconds, step=.1, what=label)
        except Exception:
            diagnostic = {"waiting_for": label}
            for peer in peers:
                try:
                    diagnostic[peer.name] = {"status": peer.bridge.status(),
                        "players": [data(peer, row["id"]) for row in _rows(peer) if not row["bot"]]}
                except Exception as error:
                    diagnostic[peer.name] = str(error)
            (Path(ctx.out_dir) / "progression_failure.json").write_text(json.dumps(diagnostic, indent=2), encoding="utf-8")
            raise

    def data(peer, player, **arguments):
        return peer.bridge.call("progression", actor=player, **arguments)

    def view(peer, player):
        return data(peer, player)["view"]

    def action(peer, player, command, key=""):
        result = data(peer, player, action=command, key=key)
        assert result["submitted"], (player, command, key, result)

    def avatar(player, peer=host):
        return next(row["avatar_id"] for row in _rows(peer) if row["id"] == player)

    def character(player, peer=host):
        return next(row for row in peer.bridge.state() if row["id"] == avatar(player, peer))

    def readout(peer):
        rows = peer.bridge.status()["progression_ui"]
        return rows[0] if rows else {}

    def arena_code(code):
        return host.bridge.call("exec", target="level", context="level_specific", code=code)

    def arena_probe(expression):
        host.bridge.call("level_messages", prefix="weapon_arena_probe ")
        arena_code('level.SendMessage("weapon_arena_probe " + (' + expression + '));')
        lines = host.bridge.call("level_messages", prefix="weapon_arena_probe ")["messages"]
        return lines[-1].split(" ", 1)[1]

    def phase():
        return int(arena_probe("int(phase)"))

    def ready():
        action(host, host_player, "ready")
        action(guest, guest_player, "ready")
        wait(lambda: not view(host, host_player).get("shop_open"), "both players close shop")

    def files(player, peer=host):
        world = peer.bridge.call("state")
        person = next(row for row in world["characters"] if row["id"] == avatar(player, peer))
        attached = {entry["common_id"] for entry in person["attachments"]}
        return {item["file"].split("/")[-1].lower() for item in world["items"]
                if item["common_id"] in attached}

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:28581")
    wait(lambda: all(len(_rows(peer)) == 2 for peer in peers), "two progression players")
    host_player = next(row["id"] for row in _rows(host) if row["peer_id"] == 255)
    guest_player = next(row["id"] for row in _rows(host) if row["peer_id"] != 255)
    _code(host, 'LobbyStart();', "menu")
    for peer in peers:
        _ready(peer, FIRST)
    _skip(host)
    for peer in peers:
        _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
              'if(Online_IsPlayerCharacter(c.GetID())){if(!c.network_proxy)c.static_char=true;}'
              'else c.Execute("this_mo.static_char=true;combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");}')
    hero = character(host_player)
    enemies = []
    for offset in (4, 7):
        enemy_id = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                                    pos=[hero["pos"][0] + offset, hero["pos"][1] + .2, hero["pos"][2]],
                                    teams="xp_scale_enemy")["object_id"]
        _code(host, 'this_mo.static_char=true;combat_allowed=false;always_unaware=true;', enemy_id)
        enemies.append(enemy_id)
    _code(host, 'ScriptParams@ goals=ReadObjectFromID(14).GetScriptParams();'
          f'goals.SetString("goal_4","defeat {enemies[0]} {enemies[1]} no_delay");'
          'goals.SetString("goal_4_pre","");goals.SetString("goal_4_post","");'
          'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);if(h.GetID()==14){'
          'h.SetIntVar("progress",4);h.SetFloatVar("win_time",-1);}}')
    wait(lambda: view(host, host_player).get("xp_scale", {}).get("encounter_expected") == 32,
         "purple scale uses the exact two-rabbit objective")
    wait(lambda: view(guest, guest_player).get("xp_scale") == view(host, host_player)["xp_scale"],
         "host XP denominators reach guest")
    time.sleep(1)  # settle ordinary first-load observation and authored activation
    initial_world = view(host, host_player)["xp_scale"]["world_remaining"]
    assert initial_world >= 32
    (Path(ctx.out_dir) / "xp_world_before.json").write_text(json.dumps({
        "initial_world": initial_world, "view": view(host, host_player), "characters": host.bridge.state(),
    }, indent=2), encoding="utf-8")
    _code(host, f'attacked_by_id={avatar(host_player)};SetKnockedOut(_unconscious);', enemies[0])
    wait(lambda: view(host, host_player)["xp_scale"]["world_remaining"] == initial_world - 16,
         "green scale loses exactly the defeated enemy bounty")
    wait(lambda: all(view(peer, player)["pooled"] == 16 for peer, player in
                     ((host, host_player), (guest, guest_player))), "real knockout awards pending XP")
    wait(lambda: math.isclose(readout(host).get("purple_fraction", 0), .5, abs_tol=.001),
         "pending purple XP is half the encounter reward")
    _code(host, 'ProgressionCheckpoint();')
    wait(lambda: math.isclose(readout(host).get("green_fraction", 0), 16 / initial_world, abs_tol=.001),
         "green fill includes the banked 16 XP in the projected final balance")

    # Award incidental as well as objective bounties in the same checkpoint.
    _code(host, 'for(int i=0;i<GetNumCharacters();++i){'
          f'ProgressionReportHit(ReadCharacter(i).GetID(),{avatar(host_player)},0,true);}}')
    def full_bars(peer, player):
        account = view(peer, player)
        ui = readout(peer)
        return (account["pooled"] > account["xp_scale"]["encounter_expected"] == 32 and
                account["xp_scale"]["world_remaining"] == 0 and
                ui["green_fraction"] == ui["purple_fraction"] == 1 and
                ui["display_pooled"] == account["pooled"])

    wait(lambda: full_bars(host, host_player) and full_bars(guest, guest_player),
         "no enemies remain and actual pooled XP exceeds the full purple bar")

    # Build a bounded purse through the normal host reward/bank eligibility.
    _code(host, f'for(int n=0;n<180;++n){{ProgressionReportHit({enemies[1]},{avatar(host_player)},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();}ProgressionBeginShop(false);')
    wait(lambda: all(view(peer, player).get("shop_open") for peer, player in
                     ((host, host_player), (guest, guest_player))), "two player shops")
    wait(lambda: readout(host).get("stats_drawn") and readout(guest).get("stats_drawn"),
         "both shop stat columns draw")
    def accumulated_rewards(peer, player):
        account = view(peer, player)
        balance = account["held"]
        remaining = account["xp_scale"]["world_remaining"]
        return (balance > initial_world and remaining > 0 and
                math.isclose(readout(peer)["green_fraction"], balance / (balance + remaining), abs_tol=.001))

    wait(lambda: accumulated_rewards(host, host_player) and accumulated_rewards(guest, guest_player),
         "both wallets can earn beyond the forecast while green fill still allows for remaining enemies")
    cards = {card["id"]: card for card in view(host, host_player)["cards"]}
    assert cards["staff"]["label"] == "Staff" and cards["staff"]["cost"] == 360
    assert cards["broadsword"]["label"] == "Broadsword" and cards["broadsword"]["cost"] == 1280
    host.bridge.call("input", controller=0, hold={"menu_scroll_down": 1}, steps=-1)
    wait(lambda: {"staff", "broadsword"} <= set(readout(host).get("visible_cards", [])),
         "new stock weapon cards are visible in the actual shop")
    time.sleep(.8)
    host.bridge.call("input", controller=0, release=["menu_scroll_down"])
    ctx.shot(host, "expanded_weapon_shop_and_stats")
    for key in ("staff", "broadsword", "health", "armor_3"):
        action(host, host_player, "buy", key)
    action(guest, guest_player, "buy", "damage")
    wait(lambda: view(host, host_player)["stats"]["health_rank"] == 1 and
         view(guest, guest_player)["stats"]["damage_rank"] == 1, "each readout shows its own upgrades")
    wait(lambda: view(host, host_player)["stats"]["armor"][3] == 1,
         "shop column previews the selected wooden forearm guard")
    assert view(host, host_player)["stats"]["player"] == host_player
    assert view(guest, guest_player)["stats"]["player"] == guest_player
    assert view(host, host_player)["stats"]["damage_rank"] == 0
    assert view(guest, guest_player)["stats"]["health_rank"] == 0
    assert view(host, host_player)["owned_staff"] and view(host, host_player)["owned_broadsword"]
    ctx.shot(host, "purchased_stats_and_armor")
    ready()
    wait(lambda: all("dogbroadsword.xml" in files(host_player, peer) for peer in peers),
         "real stock broadsword is issued on both peers")
    wait(lambda: character(host_player)["combat_stats"]["progression_health"] == 1 and
         readout(host).get("stats_drawn"), "upgraded gameplay stats column stays visible")
    wait(lambda: math.isclose(view(host, host_player)["stats"]["armor_rating"], .0325, abs_tol=.0001),
         "gameplay armor readout follows the actual worn wood plate")
    ctx.shot(host, "gameplay_stats_and_reward_bars")
    _code(host, 'ProgressionBeginShop(false);')
    action(host, host_player, "equip", "staff")
    ready()
    _code(host, 'level.SendMessage("reset");')
    _skip(host)
    wait(lambda: all("staffbasic.xml" in files(host_player, peer) and
                     "dogbroadsword.xml" not in files(host_player, peer) for peer in peers),
         "staff replaces selected broadsword correctly on campaign retry")

    # Arena uses real timers and KO callbacks. Ordinary rounds/draws continue
    # automatically; only a completed match opens a shop and waits for Ready.
    _code(host, 'LobbyEndGame();')
    wait(lambda: all(peer.bridge.status()["online"]["lobby_waiting"] for peer in peers), "return to lobby")
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);'
          f'LobbySetForceTeams(true);LobbySetPlayerTeam({host_player},1);LobbySetPlayerTeam({guest_player},2);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_timer",0);'
          'LobbyStart();', "menu")
    for peer in peers:
        _ready(peer)
    wait(lambda: phase() == 2, "Arena reaches actual first fight")
    assert arena_probe("roster.length()") == "2", "manual teams avoid temporary template fighters"
    wait(lambda: all(view(peer, player).get("xp_scale", {}).get("encounter_label") == "Current match"
                     for peer, player in ((host, host_player), (guest, guest_player))),
         "Arena purple denominator follows its actual match roster")
    arena_code('max_points=2;')
    wait(lambda: all("staffbasic.xml" in files(host_player, peer) for peer in peers),
         "staff loadout works through Arena deployment")

    def knock_guest():
        _code(guest, 'this_mo.static_char=false;invincible=false;ko_shield=0;TakeDamage(10);',
              avatar(guest_player, guest))
        wait(lambda: phase() == 3, "real Arena knockout enters winner camera")
        assert all(not view(peer, player).get("shop_open") for peer, player in
                   ((host, host_player), (guest, guest_player)))
        time.sleep(1)
        assert phase() == 3 and not view(host, host_player).get("shop_open"), \
            "shop interrupted round winner camera"

    def shop_waits_for_ready(expected_phase, selection):
        wait(lambda: phase() == expected_phase and view(host, host_player).get("shop_open"),
             "shop opens after winner camera")
        current_round = arena_probe("round_number")
        time.sleep(.5)
        assert phase() == expected_phase and arena_probe("round_number") == current_round
        action(host, host_player, "equip", selection)
        ready()
        wait(lambda: phase() == 2, "Ready releases next Arena fight")

    def next_round_without_shop():
        previous_round = arena_probe("round_number")

        def continued():
            assert all(not view(peer, player).get("shop_open") for peer, player in
                       ((host, host_player), (guest, guest_player))), "shop opened between rounds"
            return phase() == 1 and arena_probe("round_number") != previous_round

        wait(continued, "round camera leads directly to the next intro")
        wait(lambda: phase() == 2, "next round starts without Ready")

    knock_guest()
    next_round_without_shop()
    assert "staffbasic.xml" in files(host_player), "ordinary round keeps selected staff"
    arena_code('max_points=1;')
    knock_guest()
    wait(lambda: phase() == 4, "winning round enters match winner camera")
    assert not view(host, host_player).get("shop_open")
    time.sleep(1)
    assert phase() == 4 and not view(host, host_player).get("shop_open"), "shop interrupted match camera"
    shop_waits_for_ready(6, "broadsword")
    wait(lambda: all("dogbroadsword.xml" in files(host_player, peer) for peer in peers),
         "next match applies selected broadsword on both peers")
    _code(host, 'LobbyEndGame();')
    wait(lambda: all(peer.bridge.status()["online"]["lobby_waiting"] for peer in peers), "lobby before shop-disabled case")
    _code(host, 'LobbySetModeSetting("progression",0);LobbyStart();', "menu")
    for peer in peers:
        _ready(peer)
    wait(lambda: phase() == 2, "shop-disabled Arena fight")
    arena_code('max_points=2;')
    knock_guest()
    wait(lambda: phase() == 1, "shop-disabled winner timer leads directly to next intro")
    assert not data(host, host_player)["view"]["enabled"]
    wait(lambda: phase() == 2, "shop-disabled next fight starts automatically")
    assert all(not script_errors(peer) for peer in peers), [script_errors(peer) for peer in peers]
    return {
        "passed": True,
        "metrics": {"world_xp_before": initial_world, "encounter_xp": 32,
                    "weapons": ["staffbasic.xml", "DogBroadSword.xml"],
                    "player_specific_stats": True, "armor_rating": .0325,
                    "arena_match_only_shop_round_and_disabled": True},
        "notes": [],
    }
