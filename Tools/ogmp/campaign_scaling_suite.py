"""Campaign scaling, host-owned difficulty, and late/local party members.

Uses Ice Cliff's authored wolf plus ordinary rabbit fixtures. It exercises the
normal script setting API, actual lobby joins, and replicated character state.
"""

import math
import time
import traceback
from pathlib import Path

from .newcampaign_local_suite import _ready, _skip
from .scenarios import script_errors

CAMPAIGN = "com-wolfire-overgrowth-campaign"
LEVEL = "Data/Levels/og_story/08_Ice_Cliff_Landing.xml"
FACTORS = ("campaign_vitality_mult", "campaign_damage_mult", "campaign_armor_mult", "campaign_ai_bonus")
AI_FIELDS = ("p_aggression", "p_ground_aggression", "p_block_skill", "p_block_followup",
             "p_attack_speed_mult", "g_throw_counter_probability", "g_weapon_catch_skill")
AI_TRAITS = {1: "p_aggression", 2: "p_ground_aggression", 3: "p_block_followup"}
RENDER_KEYS = {"texture_reduce": "3", "multisample": "1", "detail_objects": "0",
               "particle_field": "0", "no_reflection_capture": "1", "depth_of_field": "0",
               "limit_fps_in_game": "true", "max_frame_rate": "30"}


def campaign_scaling(ctx):
    host = ctx.instance("scaling_host", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27974", "difficulty_preset_value": "3",
                        "game_difficulty": "1", "global_time_scale_mult": "1", "tutorials": "false",
                        "auto_ledge_grab": "false", "campaign_mp_scaling": "true", "difficulty_scaling": "true", "campaign_ai_scaling": "false", "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false",
                        "player_invincible": "true"})
    metrics = {}
    ctx.campaign_scaling_metrics = metrics

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def rows(inst):
        return inst.bridge.status()["online"]["players"]

    def wait(predicate, label, timeout=35):
        return host.bridge.wait(predicate, timeout, step=0.1, what=label)

    def world(inst):
        return inst.bridge.state()

    def actor(inst, common):
        return next((c for c in world(inst) if c.get("common_id") == common), None)

    def stats(inst, common):
        c = actor(inst, common)
        return c.get("combat_stats", {}) if c else {}

    def factors(inst, common):
        s = stats(inst, common)
        return tuple(s.get(k, -1) for k in FACTORS)

    def probe(inst, expression):
        inst.bridge.call("level_messages", prefix="scaling_probe")
        code(inst, 'level.SendMessage("scaling_probe " + (' + expression + '));')
        messages = inst.bridge.call("level_messages", prefix="scaling_probe")["messages"]
        assert messages, "campaign scaling script probe did not execute"
        return messages[-1].split(" ", 1)[1]

    def settings(inst):
        return probe(inst, 'GetConfigValueString("difficulty_preset") + "|" + '
                     'GetConfigValueBool("campaign_mp_scaling") + "|" + '
                     'GetConfigValueBool("campaign_ai_scaling") + "|" + '
                     'GetConfigValueFloat("game_difficulty")').split("|")

    def no_ai_change(common, baseline):
        current = stats(host, common)
        for key in AI_FIELDS:
            assert math.isclose(current[key], baseline[key], abs_tol=1e-5), (common, key, current[key], baseline[key])

    def freeze_humans(inst):
        code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(c.controlled&&!c.network_proxy)c.static_char=true;}')

    def set_encounter(originals, advance=False):
        inst = host
        inst.bridge.call("level_messages", prefix="scaling_encounter")
        goal = "defeat " + " ".join(str(n) for n in originals)
        code(inst, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
             'Object@ o=ReadObjectFromID(h.GetID());if(h.HasVar("progress")&&o.GetScriptParams().HasParam("goal_0")){'
             'int goal=h.GetIntVar("progress")+' + ("1" if advance else "0") + ';'
             'o.GetScriptParams().SetString("goal_"+goal,"' + goal + '");'
             'o.GetScriptParams().SetString("goal_"+goal+"_pre","");'
             'o.GetScriptParams().SetString("goal_"+goal+"_post","");'
             'h.SetIntVar("progress",goal);h.SetFloatVar("win_time",-1.0f);'
             'level.SendMessage("scaling_encounter "+h.GetID()+" "+goal);}}')
        messages = inst.bridge.call("level_messages", prefix="scaling_encounter")["messages"]
        assert len(messages) == 1, ("one authored encounter hotspot", messages)
        return tuple(int(v) for v in messages[0].split()[1:])

    code(host, 'LobbyBeginHost(); LobbySetSelection("' + CAMPAIGN + '","' + CAMPAIGN + '","' + LEVEL + '",false); LobbyStart();', "menu")
    _ready(host)
    _skip(host)
    freeze_humans(host)
    # Keep the authored boss alive and stationary while measuring its stats.
    code(host, 'Object@ o=ReadObjectFromID(67);o.SetEnabled(true);o.GetScriptParams().SetInt("Campaign Boss",1);'
         'ReadCharacterID(67).ReceiveScriptMessage("set_combat_allowed false");')
    boss = 67
    wait(lambda: FACTORS[0] in stats(host, boss), "campaign scaling variables on the authored wolf")
    baseline_boss = stats(host, boss).copy()
    assert factors(host, boss) == (1.0, 1.0, 1.0, 0.0), factors(host, boss)
    assert settings(host)[:3] == ["Expert", "true", "false"], settings(host)

    hero = host.bridge.me()
    base = next(c["pos"] for c in world(host) if c["id"] == hero["id"])
    regulars = []
    # Keep at least four ordinary fixtures and one deterministic AI scaling
    # candidate. Selection uses stable shared IDs, never a probabilistic roll.
    def ai_trait(shared_id):
        identity = (shared_id * 1664525 + 1013904223) & 0xffffffff
        return 0 if identity & 7 else 1 + ((identity >> 3) % 3)

    for index in range(12):
        pos = [base[0] + 2.0 + index * 0.7, base[1] + 0.2, base[2] + 2.0]
        common = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml", pos=pos, teams="scaling_enemy")["object_id"]
        code(host, f'ScriptParams@ p=ReadObjectFromID({common}).GetScriptParams();p.SetInt("Campaign Boss",0);'
             'p.SetInt("Wearing Metal Armor",1);p.SetFloat("Aggression",0.23f);p.SetFloat("Block Skill",0.31f);'
             'p.SetFloat("Attack Damage",1.2f);p.SetFloat("Damage Resistance",1.3f);'
             'p.SetFloat("Throw Counter Probability",0.17f);p.SetFloat("Weapon Catch Skill",0.27f);'
             f'ReadCharacterID({common}).Execute("SetParameters();");'
             f'ReadCharacterID({common}).ReceiveScriptMessage("set_combat_allowed false");')
        regulars.append(common)
        if len(regulars) >= 4 and any(ai_trait(n) for n in regulars):
            break
    assert any(ai_trait(n) for n in regulars), ("fixture needs a sparse shared-ID AI candidate", regulars)
    regular = regulars[0]
    baseline_regular = stats(host, regular).copy()
    assert factors(host, regular) == (1.0, 1.0, 1.0, 0.0), factors(host, regular)
    # Put the ordinary fixtures into the real current campaign encounter.
    # Only the disposable test profile's runtime parameters are changed.
    regular_encounter = set_encounter(regulars)
    metrics["regular_encounter"] = regular_encounter
    metrics["solo"] = {"boss": baseline_boss, "regular": baseline_regular}

    # The cost curve below compares successive checkpoint samples in one scene.
    def checkpoint_sample():
        code(host, 'Online_CaptureEnemyDifficulty();')

    # Equal headcounts must not imply equal difficulty: AI allies provide only
    # a quarter of an additional human's scaling contribution.
    code(host, "for(int i=0;i<3;++i)LobbyAddBot();")
    wait(lambda: len(rows(host)) == 4 and len([p for p in rows(host) if p.get("bot")]) == 3,
         "one human with three friendly bots")
    time.sleep(.8)
    assert factors(host, regular)[0] == 1, "joining allies buffed a living enemy mid-fight"
    checkpoint_sample()
    wait(lambda: math.isclose(factors(host, regular)[0], 1.1918875, abs_tol=1e-5) and
         math.isclose(factors(host, boss)[0], 1.865725, abs_tol=1e-5), "bot-heavy party uses lighter stat growth")
    bot_heavy_boss, bot_heavy_regular = factors(host, boss), factors(host, regular)
    for player in rows(host):
        if player.get("bot"):
            bot_actor = next(c for c in world(host) if c["id"] == player["avatar_id"])
            assert factors(host, bot_actor["common_id"]) == (1.0, 1.0, 1.0, 0.0), bot_actor
    wait(lambda: len([c for c in world(host) if c.get("campaign_scaling_source", -1) in regulars]) == 2,
         "three friendly bots receive two early logarithmic reinforcements")
    early_additions = {c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) in regulars}
    metrics["one_human_three_bots"] = {"boss": bot_heavy_boss, "regular": bot_heavy_regular,
                                       "reinforcements": sorted(early_additions)}
    departed_owner = next(p["avatar_id"] for p in rows(host) if p.get("bot"))
    host.bridge.call("level_messages", prefix="scaling_stale_item")
    code(host, 'int item=CreateObject("Data/Items/DogWeapons/DogSword.xml",true);'
         f'ReadItemID(item).last_held_char_id_={departed_owner};ReadItemID(item).SetThrown();'
         'ReadObjectFromID(item).SetTranslation(vec3(0,1000,0));level.SendMessage("scaling_stale_item "+item);')
    stale_item = int(host.bridge.call("level_messages", prefix="scaling_stale_item")["messages"][-1].split()[1])
    code(host, "".join(f'LobbyRemovePlayer({p["id"]});' for p in rows(host) if p.get("bot")))
    checkpoint_sample()
    wait(lambda: len(rows(host)) == 1 and all(factors(host, n) == (1.0, 1.0, 1.0, 0.0) for n in (boss, regular)),
         "removing temporary bots restores solo difficulty")
    assert probe(host, f'!MovementObjectExists({departed_owner})&&ObjectExists({stale_item})') == "true"
    host.bridge.call("level_messages", prefix="scaling_stale_hit")
    # This guard fixture declares full-body metal armor for separate tests.
    # Exercise an unarmored impact here so a valid deflection cannot hide the
    # behavior of an item that outlived its owner.
    code(host, f'ItemObject@ item=ReadItemID({stale_item});item.SetLinearVelocity(vec3(6,0,0));'
         'bool was_armored=g_wearing_metal_armor;g_wearing_metal_armor=false;'
         'this_mo.velocity=vec3(0);float old_temp=temp_health;float old_blood=blood_health;'
         f'HitByItem("metal",this_mo.position,{stale_item},1);g_wearing_metal_armor=was_armored;'
         'level.SendMessage("scaling_stale_hit "+attacked_by_id+"|"+(length(this_mo.velocity)>0.0f)+"|"+'
         '(temp_health<old_temp||blood_health<old_blood));', regular)
    stale_hit = host.bridge.call("level_messages", prefix="scaling_stale_hit")["messages"][-1].split(" ", 1)[1]
    assert stale_hit == "-1|true|true", ("departed thrower must not suppress impact damage or become an invalid AI target", stale_hit)
    code(host, f'DeleteObjectID({stale_item});ReadCharacterID({regular}).ReceiveScriptMessage("restore_health");')
    metrics["departed_thrower_impact"] = {"owner": departed_owner, "item": stale_item,
                                           "invalid_attacker_cleared": True, "impulse_and_damage_preserved": True}

    code(host, "LobbyAddLocalPlayer();")
    checkpoint_sample()
    wait(lambda: len(rows(host)) == 2 and all(factors(host, n)[0] > 1 for n in (boss, regular)), "two-local-human scaling")
    two_boss, two_regular = factors(host, boss), factors(host, regular)
    assert math.isclose(two_regular[0], 1.25585, abs_tol=1e-5), two_regular
    assert math.isclose(two_boss[0], 2.1543, abs_tol=1e-5), two_boss
    assert two_boss[0] > two_regular[0] and two_boss[1] > 1 and two_boss[2] == 1, (two_boss, two_regular)
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)
    time.sleep(0.8)  # allow multiple normal difficulty update intervals
    assert {c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) in regulars} == early_additions, \
        "a smaller party should retain the existing encounter enemies without spawning more"
    freeze_humans(host)
    metrics["two_humans"] = {"boss": two_boss, "regular": two_regular}

    # A late guest begins with different personal match preferences. The host's
    # current settings, scaling factors, and roster must replace those views.
    guest = ctx.instance("scaling_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27975", "game_difficulty": "0", "global_time_scale_mult": "0.8",
                         "tutorials": "true", "auto_ledge_grab": "true", "campaign_mp_scaling": "false", "difficulty_scaling": "true", "campaign_ai_scaling": "false", "campaign_armor_scaling": "false", "campaign_weapon_scaling": "false",
                         "player_invincible": "true"})
    guest.bridge.call("connect", ip="127.0.0.1:27974")
    _ready(guest)
    wait(lambda: len(rows(host)) == len(rows(guest)) == 3, "late guest campaign roster")
    wait(lambda: settings(guest) == settings(host), "authoritative host difficulty reaches the late guest")
    checkpoint_sample()
    wait(lambda: math.isclose(factors(host, regular)[0], 1.5117, abs_tol=1e-5) and
         math.isclose(factors(host, boss)[0], 3.3086, abs_tol=1e-5), "three-human difficulty reacts")
    assert {c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) in regulars} == early_additions, \
        "two additional humans retain the encounter's two existing reinforcements"
    code(guest, "LobbyAddLocalPlayer();")
    wait(lambda: len(rows(host)) == len(rows(guest)) == 4, "guest's second local human")
    checkpoint_sample()
    wait(lambda: math.isclose(factors(host, boss)[0], 4.3766945, abs_tol=1e-5) and
         math.isclose(factors(host, regular)[0], 1.7484426, abs_tol=1e-5), "four-human difficulty reacts")
    wait(lambda: all(factors(host, n) == factors(guest, n) for n in (boss, regular)), "late guest receives current actor scaling")
    four_boss, four_regular = factors(host, boss), factors(host, regular)
    assert math.isclose(four_regular[0], 1.7484426, abs_tol=1e-5), ("four-human rabbit durability", four_regular)
    assert math.isclose(four_boss[0], 4.3766945, abs_tol=1e-5), ("four-human boss durability", four_boss)
    assert four_regular[0] > bot_heavy_regular[0] and four_boss[0] > bot_heavy_boss[0], \
        "four humans must scale more strongly than one human with three bots"
    wait(lambda: len([c for c in world(host) if c.get("campaign_scaling_source", -1) in regulars]) == 3,
         "three additional humans receive three safely placed same-race reinforcements")
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)
    freeze_humans(host)
    freeze_humans(guest)
    metrics["four_humans"] = {"boss": four_boss, "regular": four_regular, "settings": settings(guest)}

    code(guest, 'SetConfigValueString("difficulty_preset","Casual");SetConfigValueBool("campaign_mp_scaling",false);')
    time.sleep(0.5)
    assert settings(guest) == settings(host) == ["Expert", "true", "false", "1"], (settings(host), settings(guest))
    code(host, 'SetConfigValueFloat("game_difficulty",1.1f);')
    assert settings(host)[3] == "1", settings(host)

    # Changing party size and difficulty must not heal an injured boss or
    # refill its spent knockout protection.
    code(host, 'ReadCharacterID(67).Execute("permanent_health=0.4f;temp_health=0.4f;blood_health=0.4f;ko_shield=1;");')
    code(host, 'SetConfigValueBool("campaign_mp_scaling",false);Online_CaptureEnemyDifficulty();')
    wait(lambda: all(factors(host, n) == (1.0, 1.0, 1.0, 0.0) for n in (boss, regular)), "stat scaling checkbox disables both encounters")
    wait(lambda: settings(guest)[:3] == ["Expert", "false", "false"], "checkbox is shared with the guest")
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)
    assert actor(host, boss)["permanent_health"] <= 0.4001 and stats(host, boss)["ko_shield"] == 1

    ai_baselines = {n: stats(host, n).copy() for n in [boss] + regulars}
    code(host, 'SetConfigValueBool("campaign_ai_scaling",true);Online_CaptureEnemyDifficulty();')
    wait(lambda: all(math.isclose(factors(host, n)[3], 0.01 if ai_trait(n) else 0.0, abs_tol=1e-5)
                     for n in [boss] + regulars), "AI scaling applies sparse one-percent offensive variation")
    assert factors(host, boss)[:3] == (1.0, 1.0, 1.0), factors(host, boss)
    tweaked = []
    for common, baseline in ai_baselines.items():
        current = stats(host, common)
        trait = ai_trait(common)
        field = AI_TRAITS.get(trait)
        # Explicit authored zero/one stays exact, including training actors.
        expected_trait = trait if field and 0 < baseline[field] < 1 else 0
        assert current["campaign_ai_tweak"] == expected_trait, (common, current)
        changed = [key for key in AI_FIELDS if not math.isclose(current[key], baseline[key], abs_tol=1e-5)]
        assert changed == ([field] if expected_trait else []), ("AI scaling changed unrelated AI", common, changed)
        if expected_trait:
            expected = baseline[field] + (1.0 - baseline[field]) * 0.01
            assert math.isclose(current[field], expected, abs_tol=1e-5), (common, field, current[field], expected)
            tweaked.append(common)
    assert tweaked, "selected ordinary AI fixture should have one small offensive change"
    wait(lambda: settings(guest)[:3] == ["Expert", "false", "true"], "AI scaling preference is shared independently of base difficulty")
    metrics["ai_scaling"] = {"boss": stats(host, boss), "regular": stats(host, regular), "tweaked_ids": tweaked}
    code(host, 'SetConfigValueString("difficulty_preset","Expert");SetConfigValueBool("campaign_ai_scaling",false);SetConfigValueBool("campaign_mp_scaling",true);Online_CaptureEnemyDifficulty();')
    wait(lambda: factors(host, boss) == four_boss and factors(host, regular) == four_regular, "restoring Expert does not compound stat multipliers")
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)

    code(host, "LobbyAddBot();")
    wait(lambda: any(p.get("bot") for p in rows(host)), "friendly lobby bot")
    checkpoint_sample()
    wait(lambda: factors(host, boss)[0] > four_boss[0] and factors(host, regular)[0] > four_regular[0],
         "friendly bot contributes one party member to encounter difficulty")
    party_boss, party_regular = factors(host, boss), factors(host, regular)
    assert math.isclose(party_boss[0], 4.4999942, abs_tol=1e-5), ("one friendly bot adds only modest boss difficulty", party_boss)
    assert math.isclose(party_regular[0], 1.7757719, abs_tol=1e-5), party_regular
    friendly_bot = next(p for p in rows(host) if p.get("bot"))
    friendly_bot_actor = next(c for c in world(host) if c["id"] == friendly_bot["avatar_id"])
    assert factors(host, friendly_bot_actor["common_id"]) == (1.0, 1.0, 1.0, 0.0), friendly_bot_actor
    metrics["friendly_bot_party"] = {"boss": party_boss, "regular": party_regular,
                                     "bot_factors": factors(host, friendly_bot_actor["common_id"])}
    guest.bridge.call("disconnect")
    wait(lambda: len([p for p in rows(host) if not p.get("bot")]) == 2 and not guest.bridge.status()["online"]["active"],
         "the guest's two humans leave the session")
    assert actor(host, boss)["permanent_health"] <= 0.4001 and stats(host, boss)["ko_shield"] == 1
    guest.bridge.call("connect", ip="127.0.0.1:27974")
    _ready(guest)
    wait(lambda: len([p for p in rows(host) if not p.get("bot")]) >= 3, "the guest returns")
    if len([p for p in rows(host) if not p.get("bot")]) == 3:
        code(guest, "LobbyAddLocalPlayer();")
    wait(lambda: len([p for p in rows(host) if not p.get("bot")]) == 4, "human rejoins")
    checkpoint_sample()
    wait(lambda: factors(host, boss) == party_boss and factors(host, regular) == party_regular, "same human-and-bot party restores the same factors")
    assert actor(host, boss)["permanent_health"] <= 0.4001 and stats(host, boss)["ko_shield"] == 1

    extras = [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]
    # This fixture has one ordinary encounter. The shared cap counts living
    # additions, so retained bodies must not exhaust subsequent objectives.
    assert len(extras) <= 6, ("ordinary encounter reinforcement cap", len(extras))
    for addition in extras:
        original = actor(host, addition["campaign_scaling_source"])
        assert original and addition["char_path"] == original["char_path"], ("reinforcement character changed", addition, original)
        assert addition["combat_stats"]["species"] == original["combat_stats"]["species"]
        if original["common_id"] in regulars:
            assert original["combat_stats"]["metal_armor"] and not addition["combat_stats"]["metal_armor"], \
                "reinforcement inherited whole-body armor despite its independent equipment plan"
            for key in ("campaign_authored_aggression", "campaign_authored_block_skill", "campaign_authored_throw_counter",
                        "campaign_authored_weapon_catch", "campaign_authored_damage", "campaign_authored_resistance"):
                assert math.isclose(addition["combat_stats"][key], original["combat_stats"][key], abs_tol=1e-5), (key, addition, original)
    enabled_hostiles = [c for c in world(host) if c.get("knocked_out", 0) == 0 and
                         c.get("campaign_scaling_source", -1) >= 0]
    assert len(enabled_hostiles) <= 8, ("generated hostile cap", len(enabled_hostiles))
    metrics["reinforcements"] = [{k: c[k] for k in ("common_id", "char_path", "campaign_scaling_source")} for c in extras]
    metrics["boss_health_after_roster_changes"] = actor(host, boss)["permanent_health"]

    # Temporary bots exercise boss bodyguards without additional rendered
    # views. The encounter cannot finish while its guard is awake.
    original_bot_ids = {p["id"] for p in rows(host) if p.get("bot")}
    code(host, "LobbyAddBot();LobbyAddBot();")
    checkpoint_sample()
    wait(lambda: len([p for p in rows(host) if p.get("bot")]) == 3, "weighted party for boss bodyguard coverage")
    temporary_bots = [p["id"] for p in rows(host) if p.get("bot") and p["id"] not in original_bot_ids]
    wait(lambda: math.isclose(factors(host, boss)[0], 4.71025, abs_tol=1e-5),
         "temporary friendly bots contribute their lighter party weights")
    boss_dog = host.bridge.call("spawn", path="Data/Objects/characters/dogs/dog_soldier_actor.xml",
                               pos=[base[0] + 5, base[1] + 0.2, base[2] + 1], teams="scaling_enemy")["object_id"]
    code(host, f'ReadObjectFromID({boss_dog}).GetScriptParams().SetInt("Campaign Boss",1);'
         f'ReadCharacterID({boss_dog}).ReceiveScriptMessage("set_combat_allowed false");')
    boss_encounter = set_encounter([boss_dog], advance=True)
    wait(lambda: any(c.get("campaign_scaling_source", -1) == boss_dog for c in world(host)), "boss bodyguard is placed")
    guards = [c for c in world(host) if c.get("campaign_scaling_source", -1) == boss_dog]
    assert len(guards) == 1, ("weighted party receives exactly one ordinary boss guard", guards)
    assert guards[0]["char_path"] == actor(host, boss_dog)["char_path"]
    assert factors(host, boss_dog)[0] > party_regular[0]
    assert factors(host, guards[0]["common_id"])[0] < factors(host, boss_dog)[0], "bodyguard inherited boss durability"
    code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", boss_dog)
    time.sleep(0.8)
    gate = f'""+Online_CampaignEncounterDefeated({boss_encounter[0]},{boss_encounter[1]})'
    assert probe(host, gate) == "false", "boss goal ended before its bodyguard was defeated"
    for guard in guards:
        code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", guard["id"])
    wait(lambda: probe(host, gate) == "true", "boss encounter finishes after its bodyguard")
    metrics["boss_bodyguard"] = {"encounter": boss_encounter, "boss": boss_dog, "guard": guards[0]["common_id"]}
    code(host, "".join(f"LobbyRemovePlayer({ident});" for ident in temporary_bots))
    checkpoint_sample()
    wait(lambda: len([p for p in rows(host) if p.get("bot")]) == 1 and
         factors(host, boss) == party_boss and factors(host, regular) == party_regular,
         "temporary bot removal restores the original four-human one-bot party")

    # The allowance is concurrent, so completed encounters must not consume a
    # chapter's entire budget. Keep a history after clearing fixture bodies
    # from this small landing; the former lifetime cap still fails this test.
    chapter_additions = [c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]
    for ident in regulars:
        code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", ident)
    for addition in [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]:
        code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", addition["id"])
        code(host, f'DeleteObjectID({addition["id"]});')
    code(host, f'DeleteObjectID({boss_dog});')
    later_waves = []
    for wave in range(2):
        original = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                                    pos=[base[0] + 5, base[1] + 0.2, base[2] + 1], teams="scaling_enemy")["object_id"]
        code(host, f'ReadObjectFromID({original}).GetScriptParams().SetInt("Campaign Boss",0);'
             f'ReadCharacterID({original}).ReceiveScriptMessage("set_combat_allowed false");')
        encounter = set_encounter([original], advance=True)

        def wave_extras():
            return [c for c in world(host) if c.get("campaign_scaling_source", -1) == original]

        if wave in (0, 1):
            # One frame creates at most two. Kill them and their template in
            # one script call before the reserved third can be allocated.
            host.bridge.wait(lambda: len(wave_extras()) >= 2, 25, step=0.02, what="first batch of a reserved wave")
            host.bridge.call("level_messages", prefix="scaling_queued_gate")
            code(host, 'int count=0;for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                 'ScriptParams@ p=ReadObjectFromID(c.GetID()).GetScriptParams();'
                 f'bool extra=p.HasParam("Campaign Scaling Source")&&p.GetInt("Campaign Scaling Source")=={original};'
                 f'if(c.GetID()=={original}||extra){{if(extra)++count;'
                 'c.Execute("SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);");}}'
                 f'level.SendMessage("scaling_queued_gate "+count+"|"+Online_CampaignEncounterDefeated({encounter[0]},{encounter[1]}));')
            queued = host.bridge.call("level_messages", prefix="scaling_queued_gate")["messages"]
            assert queued and queued[-1].split(" ", 1)[1] == "2|false", ("reserved third actor must gate victory", queued)
            if wave == 0:
                wait(lambda: len(wave_extras()) == 3, "reserved final reinforcement still spawns after its template is defeated")
                assert probe(host, f'Online_CampaignEncounterDefeated({encounter[0]},{encounter[1]})') == "false"
            else:
                # No new KO event occurs after this change. Cancelling the
                # reserved third must recheck the already-defeated real goal.
                code(host, f'ReadObjectFromID({encounter[0]}).GetScriptParams().SetString("goal_{encounter[1] + 1}","reach 999999");'
                     'SetConfigValueBool("campaign_mp_scaling",false);Online_CaptureEnemyDifficulty();')

                def wave_progress():
                    host.bridge.call("level_messages", prefix="scaling_recheck_progress")
                    code(host, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
                         f'if(h.GetID()=={encounter[0]})level.SendMessage("scaling_recheck_progress "+h.GetIntVar("progress"));}}')
                    messages = host.bridge.call("level_messages", prefix="scaling_recheck_progress")["messages"]
                    return int(messages[-1].split(" ", 1)[1])

                wait(lambda: wave_progress() == encounter[1] + 1,
                     "cancelling the pending guard automatically rechecks the completed story goal")
                assert len(wave_extras()) == 2, "disabled scaling still produced the cancelled third guard"
        additions = wave_extras()
        chapter_additions.extend(c["common_id"] for c in additions)
        awake_additions = [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0 and
                           c.get("enabled") and c.get("knocked_out", 0) == 0]
        assert len(awake_additions) <= 8, ("concurrent living reinforcement cap", len(awake_additions))
        if wave == 0:
            for addition in additions:
                code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", addition["id"])
            code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", original)
        wait(lambda: probe(host, f'Online_CampaignEncounterDefeated({encounter[0]},{encounter[1]})') == "true",
             "reserved wave completes only after every addition is down")
        later_waves.append({"encounter": encounter, "original": original,
                            "extras": [c["common_id"] for c in additions], "awake_peak_sample": len(awake_additions),
                            "pending_cancelled": wave == 1})
        if wave == 0:
            code(host, "".join(f'DeleteObjectID({c["id"]});' for c in additions) + f'DeleteObjectID({original});')
    assert len(chapter_additions) >= 9, ("encounters still share the old lifetime budget", chapter_additions)
    metrics["later_waves"] = later_waves
    metrics["chapter_reinforcement_births"] = len(chapter_additions)
    code(host, 'SetConfigValueBool("campaign_mp_scaling",true);Online_CaptureEnemyDifficulty();')
    wait(lambda: factors(host, boss) == party_boss and factors(host, regular) == party_regular,
         "reenabling difficulty after pending-wave cancellation restores party factors")

    old_additions = {c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) >= 0}
    before_retry = len(old_additions)
    code(host, 'level.SendMessage("reset");')
    _skip(host)
    wait(lambda: not old_additions.intersection(c["common_id"] for c in world(host)),
         "retry removes the previous attempt's reinforcements and corpses")
    wait(lambda: factors(host, boss) == party_boss and factors(host, regular) == party_regular, "retry preserves party scaling without compounding")
    after_retry = [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]
    assert len(after_retry) <= 4, ("retry accumulated reinforcements", len(after_retry))
    assert len([p for p in rows(host) if not p.get("bot")]) == 4
    metrics["retry"] = {"extra_actors_before": before_retry, "extra_actors_after": len(after_retry)}
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:6])
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
    return {"passed": True, "metrics": metrics, "notes": []}


def campaign_first_level_reinforcements(ctx):
    """Seven allied bots add enemies to White Flags' real authored fights.

    Keep the original actors, goals, dialogue and checkpoint progression.
    Checkpoint messages skip the platforming route, while combat is held so
    automated observations cannot race the party defeating an encounter.
    """
    from .newcampaign_local_suite import FIRST

    host = ctx.instance("first_level_reinforcements", width=640, height=360,
                        keys={**RENDER_KEYS, "mp_port": "28064", "difficulty_preset": "Expert",
                              "difficulty_preset_value": "3", "game_difficulty": "1",
                              "global_time_scale_mult": "1", "tutorials": "false", "auto_ledge_grab": "false",
                              "campaign_mp_scaling": "true", "difficulty_scaling": "true", "campaign_ai_scaling": "true",
                              "player_invincible": "true"})
    metrics = {}

    def code(text, target="level"):
        return host.bridge.call("exec", target=target, code=text)

    def wait(predicate, label):
        return host.bridge.wait(predicate, 35, step=0.1, what=label)

    def world():
        return host.bridge.state()

    def probe(expression, story=False):
        host.bridge.call("level_messages", prefix="first_reinforcement_probe")
        statement = 'level.SendMessage("first_reinforcement_probe "+(' + expression + '));'
        if story:
            statement = ('for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
                         'if(h.GetID()==14){' + statement + '}}')
        code(statement)
        messages = host.bridge.call("level_messages", prefix="first_reinforcement_probe")["messages"]
        assert len(messages) == 1, messages
        return messages[0].split(" ", 1)[1]

    def progress():
        return int(probe('h.GetIntVar("progress")', story=True))

    def hold_party():
        code("".join(f'ReadCharacterID({player["avatar_id"]}).static_char=true;'
                     for player in host.bridge.status()["online"]["players"] if player["avatar_id"] >= 0))

    def near(source):
        code(f'MovementObject@ enemy=ReadCharacterID({source});MovementObject@ hero=ReadCharacterID(0);'
             'hero.position=Online_SafeSpotNear(enemy.position,enemy.GetFacing(),12);'
             'hero.static_char=true;hero.Execute("FixDiscontinuity();SetOnGround(true);");')

    def checkpoint(number, hotspot, expected):
        code(f'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint {number} 0 {hotspot}");')
        wait(lambda: progress() == expected, "authored checkpoint advances to goal " + str(expected))
        _skip(host)
        hold_party()

    def additions(sources):
        return [c for c in world() if c.get("campaign_scaling_source", -1) in sources]

    def defeat(actors):
        for actor in actors:
            code('SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);', actor)

    def inspect(sources, expected, phase):
        wait(lambda: len(additions(sources)) == expected, phase + " receives its logarithmic reinforcements")
        current = {c["common_id"]: c for c in world()}
        added = additions(sources)
        for actor in added:
            original = current[actor["campaign_scaling_source"]]
            assert actor["char_path"] == original["char_path"], (phase, actor, original)
            assert actor["combat_stats"]["species"] == original["combat_stats"]["species"], (phase, actor)
            assert math.isclose(actor["combat_stats"]["campaign_vitality_mult"], 1.4477375, abs_tol=1e-5), (phase, actor)
            assert actor["combat_stats"]["campaign_ai_bonus"] == 0, "bots must not enable human-count AI scaling AI tiers"
        assert all(math.isclose(current[source]["combat_stats"]["campaign_vitality_mult"], 1.4477375, abs_tol=1e-5)
                   for source in sources), "ordinary tutorial rabbits were mistaken for bosses"
        metrics[phase] = [{k: c[k] for k in ("common_id", "campaign_scaling_source", "char_path", "pos")}
                          for c in added]
        return added

    try:
        code('LobbyBeginHost();for(int i=0;i<7;++i)LobbyAddBot();'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();', "menu")
        _ready(host, FIRST)
        _skip(host)
        wait(lambda: len(host.bridge.status()["online"]["players"]) == 8, "one human with seven friendly bots")
        hold_party()
        expected_goals = "defeat 363 no_delay|defeat 2207 70 no_delay|defeat 21"
        authored = ('ReadObjectFromID(14).GetScriptParams().GetString("goal_2")+"|"+'
                    'ReadObjectFromID(14).GetScriptParams().GetString("goal_4")+"|"+'
                    'ReadObjectFromID(14).GetScriptParams().GetString("goal_5")')
        assert probe(authored) == expected_goals
        assert not additions([363, 70, 2207, 21]), "reinforcements appeared before an authored fight"
        checkpoint(0, 477, 1)
        checkpoint(1, 85, 2)
        code('ReadCharacterID(363).ReceiveScriptMessage("set_combat_allowed false");')
        near(363)
        first = inspect([363], 3, "first_fight")
        assert not additions([70, 2207, 21]), "future encounter spawned early"
        defeat([363])
        time.sleep(0.7)
        assert progress() == 2, "first objective ignored its living reinforcements"
        assert probe('Online_CampaignEncounterDefeated(14,2)') == "false"
        defeat([c["id"] for c in first])
        wait(lambda: progress() == 3, "defeating the first guards completes the real objective")
        checkpoint(2, 1130, 4)
        # The upper bandits are activated by an authored volume beyond the
        # checkpoint. Enter it normally rather than changing their Static
        # parameters; a checkpoint message alone intentionally does not do it.
        code('MovementObject@ hero=ReadCharacterID(0);hero.static_char=false;'
             'hero.position=ReadObjectFromID(3134).GetTranslation();hero.Execute("FixDiscontinuity();");')
        wait(lambda: probe('!ReadCharacterID(70).static_char&&!ReadCharacterID(2207).static_char') == "true",
             "upper bandits activate when the player enters their authored volume")
        hold_party()
        for source in (70, 2207):
            code(f'ReadCharacterID({source}).ReceiveScriptMessage("set_combat_allowed false");')
        near(70)
        second = inspect([70, 2207], 3, "later_fight")
        # The route is over65m from the first fight: finished bodies may be
        # reclaimed, so count births across phases rather than retained corpses.
        assert len(first) + len(second) == 6, "later objective did not receive a fresh enemy budget"
        metrics["reinforcement_births"] = len(first) + len(second)
        assert not additions([21]), "a neutral civilian was duplicated as an enemy"
        assert probe(authored) == expected_goals, "the regression changed authored encounter definitions"
        for player in host.bridge.status()["online"]["players"]:
            if player.get("bot"):
                actor = next(c for c in world() if c["id"] == player["avatar_id"])
                assert tuple(actor["combat_stats"][key] for key in FACTORS) == (1.0, 1.0, 1.0, 0.0), actor
        defeat([70, 2207])
        time.sleep(0.7)
        assert progress() == 4, "later objective ignored its living reinforcements"
        defeat([c["id"] for c in second])
        wait(lambda: progress() == 5, "the second real objective completes after all guards are defeated")
        metrics["final_progress"] = progress()
        metrics["authored_goals"] = probe(authored)
        assert not script_errors(host), script_errors(host)
        assert not host.bridge.call("nan_scan").get("characters")
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        failure = traceback.format_exc()
        try:
            metrics["failure_status"] = host.bridge.status()
            metrics["failure_actors"] = [c for c in world() if c["id"] in (0, 21, 70, 363, 2207) or
                                          c.get("campaign_scaling_source", -1) >= 0]
            metrics["failure_source_flags"] = probe('ReadCharacterID(70).static_char+"|"+'
                                                    'ReadCharacterID(2207).static_char+"|"+'
                                                    'ReadCharacterID(70).GetBoolVar("dialogue_control")')
        except Exception:
            pass
        return {"passed": False, "metrics": metrics, "notes": [failure]}
    finally:
        host.stop()
        (Path(ctx.out_dir) / (host.name + ".log")).write_text(host.log())
