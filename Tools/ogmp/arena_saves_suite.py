"""Arena cards retain unit scores, selected battle and pending shopping boundary."""
import json
import shutil
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP
from .newcampaign_local_suite import _code, _ready, _rows
from .scenarios import script_errors


def arena_saves(ctx):
    keys = {**RENDER_KEYS, "oo_arena_random": "false", "player_invincible": "true",
            "difficulty_scaling": "false"}
    host = ctx.instance("arena_save_host", width=640, height=360, keys={**keys, "mp_port": "28691"})
    guest = ctx.instance("arena_save_guest", width=640, height=360, keys={**keys, "mp_port": "28692"})
    wait = lambda fn, label, seconds=45: host.bridge.wait(fn, seconds, step=.1, what=label)

    def menu(peer, text):
        return _code(peer, text, "menu")

    def arena(peer, text):
        return peer.bridge.call("exec", target="level", context="level_specific", code=text)

    def probe(peer, expression):
        peer.bridge.call("level_messages", prefix="arena_save_probe")
        arena(peer, 'level.SendMessage("arena_save_probe " + (' + expression + '));')
        rows = peer.bridge.call("level_messages", prefix="arena_save_probe")["messages"]
        assert rows, expression
        return rows[-1].split(" ", 1)[1]

    def card(peer, slot):
        return Path(peer.writedir, "Data", "SessionSaves", f"slot{slot}.json")

    def save(peer, slot):
        old = card(peer, slot).read_text() if card(peer, slot).exists() else ""
        _code(peer, f"SessionSave({slot});")
        wait(lambda: card(peer, slot).exists() and card(peer, slot).read_text() != old, "Arena card written")
        return json.loads(card(peer, slot).read_text())

    def slot_defs(peer):
        return peer.bridge.call("session_slots")["saved"]

    def avatar(peer, player):
        return next(p["avatar_id"] for p in _rows(peer) if p["id"] == player)

    def freeze(*peers):
        for peer in peers:
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                  'if(!c.network_proxy){c.static_char=true;c.ReceiveScriptMessage("set_combat_allowed false");}}')

    def match(peer):
        result = {"round": int(probe(peer, '""+round_number')),
                  "max_points": int(probe(peer, '""+max_points')),
                  "battle": probe(peer, "battle_name"), "fighters": {}}
        peer.bridge.call("level_messages", prefix="arena_save_fighter")
        arena(peer, 'for(uint i=0;i<roster.length();++i)level.SendMessage("arena_save_fighter "+'
              'ArenaSeatKey(roster[i])+" "+points[i]+" "+fighter_teams[i]+" "+fighter_spots[i]);')
        for line in peer.bridge.call("level_messages", prefix="arena_save_fighter")["messages"]:
            key, points, team, spot = line.split()[1:]
            result["fighters"][key] = (int(points), int(team), int(spot))
        return result

    def expected(record):
        value = record["world"]["arena"]
        return {"round": value["round"], "max_points": value["max_points"], "battle": value["battle"],
                "fighters": {r["key"]: (r["points"], r["team"], r["spot"]) for r in value["fighters"]}}

    def ready_shop(peers):
        for peer in peers:
            local_avatar = peer.bridge.me()["id"]
            for row in _rows(peer):
                if not row["bot"] and row["avatar_id"] == local_avatar:
                    assert peer.bridge.call("progression", actor=row["id"], action="ready")["submitted"]

    menu(host, "LobbyBeginHost();")
    guest.bridge.call("connect", ip="127.0.0.1:28691")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "Arena two humans connected")
    owner = next(p["id"] for p in _rows(host) if p["host"])
    visitor = next(p["id"] for p in _rows(host) if not p["host"])
    bot = host.bridge.call("session_slots", action="add")["created"]
    empty = host.bridge.call("session_slots", action="add")["created"]
    assert host.bridge.call("session_slots", action="kind", slot=empty, player=True)["submitted"]
    removed = host.bridge.call("session_slots", action="add")["created"]
    menu(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);LobbySetForceTeams(true);'
         f'LobbySetPlayerTeam({owner},5);LobbySetPlayerTeam({visitor},2);LobbySetPlayerTeam({bot},2);'
         f'LobbySetPlayerTeam({removed},2);'
         'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",1);'
         'LobbySetModeSetting("progression_gain_damage",1);LobbySetModeSetting("progression_loss_damage",1);LobbyStart();')
    for peer in (host, guest):
        _ready(peer, ARENA_MAP)
    _code(host, f"LobbyRemovePlayer({removed});")
    wait(lambda: len(slot_defs(host)["slots"]) == 4, "live removal leaves four slots and sparse Team5")
    assert next(p["team"] for p in _rows(host) if p["id"] == owner) == 5
    wait(lambda: probe(host, '""+int(phase)') == "2", "Arena first fight")
    freeze(host, guest)
    _code(guest, 'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);', avatar(guest, visitor))
    wait(lambda: next(c for c in host.bridge.state() if c["id"] == avatar(host, visitor))["knocked_out"] != 0,
         "remote knockout reaches host before simultaneous draw")
    _code(host, f'ReadCharacterID({avatar(host, owner)}).Execute("SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);");'
          f'ReadCharacterID({avatar(host, bot)}).Execute("SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);");')
    wait(lambda: probe(host, '""+int(phase)') == "3", "draw reaches round boundary")
    assert probe(host, '""+round_winner') == "-1"
    assert probe(host, '""+ProgressionShopOpen()') == "false", "draws must not open a shop"
    wait(lambda: probe(host, '""+round_number') == "2", "draw advances without shopping")
    assert all(row[0] == 0 for row in match(host)["fighters"].values())
    wait(lambda: probe(host, '""+int(phase)') == "2", "second fight after draw")
    freeze(host, guest)
    arena(host, 'round_number=4;max_points=5;setup_points=5;'
          'for(uint i=0;i<spot_weapons.length();++i)spot_weapons[i]="Data/Items/DogWeapons/DogSword.xml";'
          f'for(uint i=0;i<roster.length();++i)points[i]=ArenaSeatKey(roster[i])=="seat{owner}"?2:1;'
          'PublishArenaState();SendScore();')
    _code(host, f'ProgressionReportHit({avatar(host, bot)},{avatar(host, owner)},.5f,false);')
    unbanked = host.bridge.call("progression", actor=owner)["view"]["pooled"]
    assert unbanked > 0
    ongoing = save(host, 1)
    original_slots = ongoing["slots"]
    assert ongoing["world"]["round"] == 4 and ongoing["world"]["points"][f"seat{owner}"] == 2
    assert all(r["weapon"].endswith("DogSword.xml") for r in ongoing["world"]["arena"]["spots"])
    # Use the owner simulation and the ordinary victory handler, rather than
    # manufacturing the post-win score or advancing its shopping phase.
    _code(host, f'ProgressionReportHit({avatar(host, bot)},{avatar(host, owner)},0,true);')
    _code(guest, 'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);', avatar(guest, visitor))
    _code(host, 'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);', avatar(host, bot))
    wait(lambda: probe(host, '""+int(phase)') == "3", "round win records current score")
    assert probe(host, '""+ProgressionShopOpen()') == "false", "non-final rounds must not open a shop"
    after_win = save(host, 2)
    assert after_win["world"]["points"][f"seat{owner}"] == 3, after_win["world"]
    shared_wallet = next(team["w"] for team in after_win["session_mods"]["og-online-progression"]["runtime"]["teams"] if team["team"] == 5)
    assert shared_wallet["held"] > 0, shared_wallet
    # Load the earlier in-progress card into this live lobby. Connected humans
    # retain their physical inputs and their unit's score and gear template.
    legacy_rules = json.loads(json.dumps(ongoing))
    legacy_rules["session_mods"]["og-online-progression"]["version"] = "1"
    legacy_rules["rules"]["mode_settings"][ARENA]["progression_combat"] = 1
    card(host, 1).write_text(json.dumps(legacy_rules))
    _code(host, "SessionLoad(1);")
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)), "manual Arena card reopens lobby")
    assert slot_defs(host) == original_slots
    menu(host, "LobbyStart();")
    for peer in (host, guest):
        _ready(peer, ARENA_MAP)
    wait(lambda: probe(host, '""+round_number') == "4", "saved Arena round restored")
    freeze(host, guest)
    assert match(host) == expected(ongoing), (match(host), expected(ongoing))
    assert host.bridge.call("progression", actor=owner)["view"]["pooled"] == unbanked
    reissued = save(host, 3)
    assert reissued["world"]["arena"]["spots"] == ongoing["world"]["arena"]["spots"]
    prefs = reissued["rules"]["mode_settings"][ARENA]
    assert "progression_combat" not in prefs and reissued["session_mods"]["og-online-progression"]["version"] == "5"
    assert (prefs["progression_gain_damage"], prefs["progression_loss_damage"], prefs["progression_gain_kill"]) == (1, 1, 0)
    # A fresh process receives only disk cards, no old session or object IDs.
    cold = ctx.instance("arena_save_cold", width=640, height=360, keys={**keys, "mp_port": "28693"})
    for folder in ("SessionSaves", "Textures/SessionSaves"):
        shutil.copytree(Path(host.writedir, "Data", folder), Path(cold.writedir, "Data", folder), dirs_exist_ok=True)
    # Compatibility with a pre-correction card that opened an inter-round shop:
    # the stale shop/ready timer must be cleared before the restored lobby.
    legacy = json.loads(json.dumps(after_win))
    legacy["version"] = 1
    mods = legacy.pop("session_mods")
    legacy["progression"] = mods["og-online-progression"]["runtime"]
    legacy["rules"]["challenge"] = mods["og-online-scaling"]["runtime"]
    legacy["rules"]["challenge"]["modes"] = legacy["rules"].pop("mode_settings")
    legacy["rules"]["challenge"]["modes"][ARENA]["progression_combat"] = 0
    legacy["world"]["arena"]["phase"] = 5
    legacy["progression"]["open"] = True
    legacy["progression"]["ms_left"] = 10000.0
    legacy["progression"]["ready"] = [owner]
    card(cold, 5).write_text(json.dumps(legacy))
    menu(cold, "SessionLoad(5);")
    wait(lambda: cold.bridge.status()["online"]["lobby_waiting"] and len(_rows(cold)) == 2,
         "cold Arena card opens host/bot and absent human placeholders")
    assert slot_defs(cold) == original_slots
    fresh = ctx.instance("arena_save_fresh_guest", width=640, height=360, keys={**keys, "mp_port": "28694"})
    fresh.bridge.call("connect", ip="127.0.0.1:28693")
    wait(lambda: any(p["id"] == visitor for p in _rows(fresh)), "new human claims saved visitor unit")
    menu(cold, "LobbyStart();")
    for peer in (cold, fresh):
        _ready(peer, ARENA_MAP)
    wait(lambda: probe(cold, '""+int(phase)') == "3", "saved round-win boundary restores without shop")
    assert probe(cold, '""+LobbyGetModeSetting("progression_gain_kill")') == "1"
    assert probe(cold, '""+LobbyGetModeSetting("progression_gain_damage")') == "0"
    assert probe(cold, 'LobbyGetModeSetting("progression_shop") == 1 && LobbyGetModeSetting("progression_free_buffs") == 0') == "true"
    assert match(cold) == expected(after_win), (match(cold), expected(after_win))
    freeze(cold, fresh)
    assert not cold.bridge.call("progression", actor=owner)["view"]["shop_open"]
    restored_team = next(team for team in cold.bridge.call("progression", actor=owner)["ledger"]["teams"] if team["team"] == 5)
    assert restored_team["w"] == shared_wallet
    assert next(p["team"] for p in _rows(cold) if p["id"] == owner) == 5
    wait(lambda: probe(cold, '""+round_number') == "5", "saved round winner advances once without shopping")
    assert match(cold)["fighters"][f"seat{owner}"][0] == 3
    assert int(probe(cold, '""+max_points')) == 5
    ctx.shot(cold, "restored_round_score")
    wait(lambda: probe(cold, '""+int(phase)') == "2", "restored fifth round begins")
    freeze(cold, fresh)
    arena(cold, "max_points=4;PublishArenaState();")
    _code(cold, f'ProgressionReportHit({avatar(cold, bot)},{avatar(cold, owner)},0,true);')
    _code(fresh, 'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);', avatar(fresh, visitor))
    _code(cold, 'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);', avatar(cold, bot))
    wait(lambda: probe(cold, '""+int(phase)') == "6", "final match victory enters shop")
    final = save(cold, 4)
    assert final["world"]["points"][f"seat{owner}"] == 4
    wallet = next(p["w"] for p in final["session_mods"]["og-online-progression"]["runtime"]["teams"] if p["team"] == 5)
    assert wallet["held"] > 0, wallet
    for collection in ("fighters", "spots", "items"):
        corrupt = json.loads(json.dumps(final))
        corrupt["world"]["arena"][collection] = [42]
        card(cold, 6).write_text(json.dumps(corrupt))
        assert probe(cold, '""+SessionLoad(6)') == "false", ("invalid Arena row must fail before ending the match", collection)
        assert cold.bridge.status()["level_loaded"] and probe(cold, '""+int(phase)') == "6"
    # Schema 2 cards had a paid shop and no free choices. Its open shop must
    # remain usable after loading the card alongside another human peer.
    old_shop = json.loads(json.dumps(final))
    old_shop["session_mods"]["og-online-progression"]["version"] = "2"
    for key in ("progression_shop", "progression_free_buffs"):
        old_shop["rules"]["mode_settings"][ARENA].pop(key, None)
    card(cold, 4).write_text(json.dumps(old_shop))
    _code(cold, "SessionLoad(4);")
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (cold, fresh)), "final-win card reopens lobby")
    menu(cold, "LobbyStart();")
    for peer in (cold, fresh):
        _ready(peer, ARENA_MAP)
    wait(lambda: probe(cold, '""+int(phase)') == "6", "saved match-win shop restored")
    assert match(cold) == expected(final)
    assert cold.bridge.call("progression", actor=owner)["view"]["held"] == wallet["held"]
    ready_shop((cold, fresh))
    wait(lambda: probe(cold, '""+round_number') == "1", "match-win shop starts exactly one fresh match")
    assert all(row[0] == 0 for row in match(cold)["fighters"].values())
    for peer in (host, guest, cold, fresh):
        assert not script_errors(peer), (peer.name, script_errors(peer))
    return {"passed": True, "metrics": {"unit_slots": len(original_slots["slots"]),
            "restored_round": 4, "next_round": 5, "winner_points": 3, "points_to_win": 5,
            "match_win_resumes_new_match": True, "restored_held_xp": wallet["held"]}, "notes": []}
