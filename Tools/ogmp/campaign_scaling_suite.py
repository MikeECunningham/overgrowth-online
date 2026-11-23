"""Campaign scaling, host-owned difficulty, and late/local party members.

Uses Ice Cliff's authored wolf plus ordinary rabbit fixtures. It exercises the
normal script setting API, actual lobby joins, and replicated character state.
"""

import math
import time

from .newcampaign_local_suite import _ready, _skip
from .scenarios import script_errors

CAMPAIGN = "com-wolfire-overgrowth-campaign"
LEVEL = "Data/Levels/og_story/08_Ice_Cliff_Landing.xml"
FACTORS = ("campaign_vitality_mult", "campaign_damage_mult", "campaign_armor_mult", "campaign_ai_bonus")
AI_FIELDS = ("p_aggression", "p_ground_aggression", "p_block_skill", "p_block_followup",
             "p_attack_speed_mult", "g_throw_counter_probability", "g_weapon_catch_skill")
RENDER_KEYS = {"texture_reduce": "3", "multisample": "1", "detail_objects": "0",
               "particle_field": "0", "no_reflection_capture": "1", "depth_of_field": "0",
               "limit_fps_in_game": "true", "max_frame_rate": "30"}


def campaign_scaling(ctx):
    host = ctx.instance("scaling_host", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27974", "difficulty_preset_value": "3",
                        "game_difficulty": "1", "global_time_scale_mult": "1", "tutorials": "false",
                        "auto_ledge_grab": "false", "campaign_mp_scaling": "true", "campaign_mp_nightmare": "false",
                        "player_invincible": "true"})
    metrics = {}

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
                     'GetConfigValueBool("campaign_mp_nightmare") + "|" + '
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
             'h.SetIntVar("progress",goal);level.SendMessage("scaling_encounter "+h.GetID()+" "+goal);}}')
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
    for index in range(4):
        pos = [base[0] + 2.0 + index * 0.7, base[1] + 0.2, base[2] + 2.0]
        common = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml", pos=pos, teams="scaling_enemy")["object_id"]
        code(host, f'ScriptParams@ p=ReadObjectFromID({common}).GetScriptParams();p.SetInt("Campaign Boss",0);'
             'p.SetInt("Wearing Metal Armor",1);p.SetFloat("Aggression",0.23f);p.SetFloat("Block Skill",0.31f);'
             'p.SetFloat("Attack Damage",1.2f);p.SetFloat("Damage Resistance",1.3f);'
             'p.SetFloat("Throw Counter Probability",0.17f);p.SetFloat("Weapon Catch Skill",0.27f);'
             f'ReadCharacterID({common}).Execute("SetParameters();");'
             f'ReadCharacterID({common}).ReceiveScriptMessage("set_combat_allowed false");')
        regulars.append(common)
    regular = regulars[0]
    baseline_regular = stats(host, regular).copy()
    assert factors(host, regular) == (1.0, 1.0, 1.0, 0.0), factors(host, regular)
    # Put the ordinary fixtures into the real current campaign encounter.
    # Only the disposable test profile's runtime parameters are changed.
    regular_encounter = set_encounter(regulars)
    metrics["regular_encounter"] = regular_encounter
    metrics["solo"] = {"boss": baseline_boss, "regular": baseline_regular}

    code(host, "LobbyAddLocalPlayer();")
    wait(lambda: len(rows(host)) == 2 and all(factors(host, n)[0] > 1 for n in (boss, regular)), "two-local-human scaling")
    two_boss, two_regular = factors(host, boss), factors(host, regular)
    assert two_boss[0] > two_regular[0] and two_boss[1] > 1 and two_boss[2] > 1, (two_boss, two_regular)
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)
    wait(lambda: any(c.get("campaign_scaling_source", -1) in regulars for c in world(host)),
         "active rabbit encounter receives a safely placed same-race reinforcement")
    freeze_humans(host)
    metrics["two_humans"] = {"boss": two_boss, "regular": two_regular}

    # A late guest begins with different personal match preferences. The host's
    # current settings, scaling factors, and roster must replace those views.
    guest = ctx.instance("scaling_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27975", "game_difficulty": "0", "global_time_scale_mult": "0.8",
                         "tutorials": "true", "auto_ledge_grab": "true", "campaign_mp_scaling": "false", "campaign_mp_nightmare": "false",
                         "player_invincible": "true"})
    guest.bridge.call("connect", ip="127.0.0.1:27974")
    _ready(guest)
    wait(lambda: len(rows(host)) == len(rows(guest)) == 3, "late guest campaign roster")
    wait(lambda: settings(guest) == settings(host), "authoritative host difficulty reaches the late guest")
    code(guest, "LobbyAddLocalPlayer();")
    wait(lambda: len(rows(host)) == len(rows(guest)) == 4, "guest's second local human")
    wait(lambda: factors(host, boss)[0] > two_boss[0] and factors(host, regular)[0] > two_regular[0], "four-human difficulty reacts")
    wait(lambda: all(factors(host, n) == factors(guest, n) for n in (boss, regular)), "late guest receives current actor scaling")
    four_boss, four_regular = factors(host, boss), factors(host, regular)
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
    code(host, 'SetConfigValueBool("campaign_mp_scaling",false);')
    wait(lambda: all(factors(host, n) == (1.0, 1.0, 1.0, 0.0) for n in (boss, regular)), "stat scaling checkbox disables both encounters")
    wait(lambda: settings(guest)[:3] == ["Expert", "false", "false"], "checkbox is shared with the guest")
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)
    assert actor(host, boss)["permanent_health"] <= 0.4001 and stats(host, boss)["ko_shield"] == 1

    code(host, 'SetConfigValueString("difficulty_preset","Nightmare");')
    wait(lambda: all(factors(host, n)[3] > 0 for n in (boss, regular)), "Nightmare adds player-count AI tuning")
    assert factors(host, boss)[:3] == (1.0, 1.0, 1.0), factors(host, boss)
    assert stats(host, regular)["p_aggression"] > baseline_regular["p_aggression"], stats(host, regular)
    wait(lambda: settings(guest)[:3] == ["Nightmare", "false", "true"], "fourth difficulty is shared")
    metrics["nightmare"] = {"boss": stats(host, boss), "regular": stats(host, regular)}
    code(host, 'SetConfigValueString("difficulty_preset","Expert");SetConfigValueBool("campaign_mp_scaling",true);')
    wait(lambda: factors(host, boss) == four_boss and factors(host, regular) == four_regular, "restoring Expert does not compound stat multipliers")
    no_ai_change(boss, baseline_boss)
    no_ai_change(regular, baseline_regular)

    code(host, "LobbyAddBot();")
    wait(lambda: any(p.get("bot") for p in rows(host)), "friendly lobby bot")
    time.sleep(1.0)
    assert factors(host, boss) == four_boss and factors(host, regular) == four_regular, "bots inflated human scaling"
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
    wait(lambda: factors(host, boss) == four_boss and factors(host, regular) == four_regular, "same party size restores the same factors")
    assert actor(host, boss)["permanent_health"] <= 0.4001 and stats(host, boss)["ko_shield"] == 1

    extras = [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]
    assert len(extras) <= 8, ("level reinforcement cap", len(extras))
    for addition in extras:
        original = actor(host, addition["campaign_scaling_source"])
        assert original and addition["char_path"] == original["char_path"], ("reinforcement character changed", addition, original)
        assert addition["combat_stats"]["species"] == original["combat_stats"]["species"]
        if original["common_id"] in regulars:
            assert addition["combat_stats"]["metal_armor"] == original["combat_stats"]["metal_armor"] is True
            for key in ("campaign_authored_aggression", "campaign_authored_block_skill", "campaign_authored_throw_counter",
                        "campaign_authored_weapon_catch", "campaign_authored_damage", "campaign_authored_resistance"):
                assert math.isclose(addition["combat_stats"][key], original["combat_stats"][key], abs_tol=1e-5), (key, addition, original)
    assert len(world(host)) <= 32, ("overall actor cap", len(world(host)))
    metrics["reinforcements"] = [{k: c[k] for k in ("common_id", "char_path", "campaign_scaling_source")} for c in extras]
    metrics["boss_health_after_roster_changes"] = actor(host, boss)["permanent_health"]

    # A separate explicit dog boss encounter gets one same-race
    # bodyguard at four humans. It cannot finish while that bodyguard is awake.
    boss_dog = host.bridge.call("spawn", path="Data/Objects/characters/dogs/dog_soldier_actor.xml",
                               pos=[base[0] + 5, base[1] + 0.2, base[2] + 1], teams="scaling_enemy")["object_id"]
    code(host, f'ReadObjectFromID({boss_dog}).GetScriptParams().SetInt("Campaign Boss",1);'
         f'ReadCharacterID({boss_dog}).ReceiveScriptMessage("set_combat_allowed false");')
    boss_encounter = set_encounter([boss_dog], advance=True)
    wait(lambda: any(c.get("campaign_scaling_source", -1) == boss_dog for c in world(host)), "boss bodyguard is placed")
    guards = [c for c in world(host) if c.get("campaign_scaling_source", -1) == boss_dog]
    assert len(guards) == 1, ("four-human boss bodyguard cap", guards)
    assert guards[0]["char_path"] == actor(host, boss_dog)["char_path"]
    assert factors(host, boss_dog)[0] > four_regular[0]
    assert factors(host, guards[0]["common_id"])[0] < factors(host, boss_dog)[0], "bodyguard inherited boss durability"
    code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", boss_dog)
    time.sleep(0.8)
    gate = f'""+Online_CampaignEncounterDefeated({boss_encounter[0]},{boss_encounter[1]})'
    assert probe(host, gate) == "false", "boss goal ended before its bodyguard was defeated"
    for guard in guards:
        code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", guard["id"])
    wait(lambda: probe(host, gate) == "true", "boss encounter finishes after its bodyguard")
    metrics["boss_bodyguard"] = {"encounter": boss_encounter, "boss": boss_dog, "guard": guards[0]["common_id"]}
    old_additions = {c["common_id"] for c in world(host) if c.get("campaign_scaling_source", -1) >= 0}
    before_retry = len(old_additions)
    code(host, 'level.SendMessage("reset");')
    _skip(host)
    wait(lambda: not old_additions.intersection(c["common_id"] for c in world(host)),
         "retry removes the previous attempt's reinforcements and corpses")
    wait(lambda: factors(host, boss) == four_boss and factors(host, regular) == four_regular, "retry preserves party scaling without compounding")
    after_retry = [c for c in world(host) if c.get("campaign_scaling_source", -1) >= 0]
    assert len(after_retry) <= 4, ("retry accumulated reinforcements", len(after_retry))
    assert len([p for p in rows(host) if not p.get("bot")]) == 4
    metrics["retry"] = {"extra_actors_before": before_retry, "extra_actors_after": len(after_retry)}
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:6])
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
    return {"passed": True, "metrics": metrics, "notes": []}
