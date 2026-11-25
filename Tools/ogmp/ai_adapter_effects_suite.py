"""Exercise the bundled adapters against installed, unmodified third-party mods.

Run with PYTHONPATH=Tools python -m ogmp.ai_adapter_effects_suite
and pass --overhaul, --dynamic and --smarter paths to the three mod directories.
"""
import argparse
import math
from pathlib import Path
import tempfile
import time

from . import instance
from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows, _skip
from .scenarios import script_errors


ADAPTER = "Data/Scripts/scaling_ai_adapters.as"
RECOVERY = "Data/Scripts/SeansGetUp/getupafterknockdown.as"


def verify_overhaul_xp(host, hero):
    seat = next(row["id"] for row in _rows(host) if row["host"])
    pos = hero["pos"]
    victim = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                              pos=[pos[0]+4, pos[1]+.25, pos[2]+3], teams="xp_enemy")["object_id"]

    def pooled():
        return host.bridge.call("progression", actor=seat)["view"]["pooled"]

    def act(script):
        host.bridge.call("exec", target=victim, code=script)

    act("this_mo.static_char=true;invincible=false;ko_shield=0;temp_health=1;permanent_health=1;blood_health=1;")
    time.sleep(.3)
    before = pooled()
    act(f'attacked_by_id={hero["id"]};TakeDamage(0.30f);')
    host.bridge.wait(lambda: pooled() > before, 8, what="Overhaul nonlethal hit earned XP")
    after_hit = pooled()
    act(f'attacked_by_id={hero["id"]};SetKnockedOut(_unconscious);')
    host.bridge.wait(lambda: pooled() > after_hit, 8, what="Overhaul KO earned XP")
    after_ko = pooled()
    act(f'attacked_by_id={hero["id"]};SetKnockedOut(_dead);')
    time.sleep(.3)
    assert pooled() == after_ko, "the already defeated opponent paid out twice"


def verify(host, which):
    def probe(expression, hook=None):
        return _probe(host, expression, "level", hook)

    def run(code, hook=None):
        return _code(host, code, "level", hook)

    def wait(predicate, label):
        return host.bridge.wait(predicate, 20, step=.15, what=label)

    hero = host.bridge.me()
    if which == "overhaul":
        verify_overhaul_xp(host, hero)
    pos = [hero["pos"][0] + 3, hero["pos"][1] + .25, hero["pos"][2] + 2]
    guard = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                             pos=pos, teams="adapter_enemy")["object_id"]
    actor = f"ReadCharacterID({guard})"
    params = f"ReadObjectFromID({guard}).GetScriptParams()"
    hero_params = f'ReadObjectFromID({hero["id"]}).GetScriptParams()'
    wait(lambda: probe(f"Online_IsDifficultyEnemy({guard})") == "true", "hostile actor recognized")
    assert probe(f'Online_IsDifficultyEnemy({hero["id"]})') == "false"
    run(f'{actor}.static_char=true;')
    if which == "overhaul":
        run(f'{params}.SetFloat("AI - Jump Kicks",0.0f);{params}.SetFloat("AI - Air Combat",0.5f);'
            f'{params}.SetFloat("AI - Feints",0.3f);{params}.SetFloat("AI - Weapon Throwing",0.99f);'
            f'ReadObjectFromID({guard}).UpdateScriptParams();')
    assert float(probe("TacticsBoost()", ADAPTER)) == 0
    for i in range(18):
        run(f'Online_ChallengeEncounterBegin("adapter_{i}");Online_ChallengeEncounterWin("adapter_{i}");')
    assert float(probe("Online_ChallengePressure()")) > .5
    assert float(probe("TacticsBoost()", ADAPTER)) == 0, "living fighters saw a live change"
    run("Online_CaptureEnemyDifficulty();")
    boost = float(probe("TacticsBoost()", ADAPTER))
    assert 0 < boost < 1
    factor = 1 + boost

    if which == "overhaul":
        for field, base in [("p_aio_jump_kicks", 0), ("p_aio_air_combat", .5),
                            ("p_aio_feints", .3), ("p_aio_throwing", .99)]:
            expected = min(1, base * factor)
            wait(lambda: math.isclose(float(probe(f'{actor}.GetFloatVar("{field}")')), expected, abs_tol=.0002),
                 f"scaled {field}")
        for _ in range(5):
            run("ScanFighters(TacticsBoost());", ADAPTER)
        assert math.isclose(float(probe(f'{actor}.GetFloatVar("p_aio_air_combat")')), .5 * factor, abs_tol=.0002)
        assert float(probe(f'{params}.GetFloat("AI - Air Combat")')) == .5
    else:
        run(f'{actor}.SetIntVar("max_defense_wait",9);{actor}.SetIntVar("defense_wait",0);'
            f'{actor}.SetIntVar("group_wait",0);{actor}.SetBoolVar("group_fighting_wait",true);'
            'ScanFighters(TacticsBoost());'
            f'for(int i=0;i<5;i++){{ {actor}.SetIntVar("group_wait",{actor}.GetIntVar("group_wait")+1);'
            f'{actor}.SetIntVar("defense_wait",{actor}.GetIntVar("defense_wait")+1); ScanFighters(TacticsBoost()); }}', ADAPTER)
        assert 5 < int(probe(f'{actor}.GetIntVar("group_wait")')) <= 10
        assert 5 < int(probe(f'{actor}.GetIntVar("defense_wait")')) <= 8
        assert int(probe(f'{actor}.GetIntVar("max_defense_wait")')) == 9

    run('global_wakeup_time_min = 7000; global_wakeup_time_max = 11000;', RECOVERY)
    assert probe(f'{params}.HasParam("MinKoTime")') == "false"
    assert probe(f'{params}.HasParam("MinKoTime")', RECOVERY) == "true"
    assert probe(f'{hero_params}.HasParam("MinKoTime")', RECOVERY) == "false"
    assert math.isclose(float(probe(f'{params}.GetFloat("MinKoTime")', RECOVERY)), 70 / factor, abs_tol=.002)
    assert math.isclose(float(probe(f'{params}.GetFloat("MaxKoTime")', RECOVERY)), 110 / factor, abs_tol=.002)
    run(f'{params}.SetFloat("MinKoTime", 9);{params}.SetFloat("MaxKoTime", 13);{params}.SetFloat("WakeupTime", 6);')
    assert float(probe(f'{params}.GetFloat("WakeupTime")', RECOVERY)) == 6
    assert math.isclose(float(probe(f'{params}.GetFloat("MinKoTime")', RECOVERY)), 9 / factor, abs_tol=.002)
    assert math.isclose(float(probe(f'{params}.GetFloat("MaxKoTime")', RECOVERY)), 13 / factor, abs_tol=.002)
    assert float(probe(f'{params}.GetFloat("MinKoTime")')) == 9
    run(f'{params}.Remove("WakeupTime");{params}.Remove("MaxKoTime");')
    assert math.isclose(float(probe(f'{params}.GetFloat("MinKoTime")', RECOVERY)), 70 / factor, abs_tol=.002)
    # Trigger the actual upstream message handler, including its random draw.
    run(f'{actor}.SetIntVar("knocked_out",_unconscious);ReceiveMessage("character_knocked_out {guard}");'
        'SetConfigValueFloat("test_adapter_duration",tracked_characters[tracked_characters.length()-1].wakeup_time);', RECOVERY)
    duration = float(probe('GetConfigValueFloat("test_adapter_duration")'))
    assert 7000 / factor - .01 <= duration <= 11000 / factor + .01
    run('SetConfigValueBool("campaign_ai_scaling",false);')
    assert math.isclose(float(probe("TacticsBoost()", ADAPTER)), boost, abs_tol=.0001)
    run("Online_CaptureEnemyDifficulty();")
    assert probe(f'{params}.HasParam("MaxKoTime")', RECOVERY) == "false"
    assert float(probe(f'{params}.GetFloat("MinKoTime")', RECOVERY)) == 9
    if which == "overhaul":
        wait(lambda: float(probe(f'{actor}.GetFloatVar("p_aio_air_combat")')) == .5, "authored input restored")
    assert not script_errors(host), script_errors(host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("overhaul", "dynamic", "smarter"):
        parser.add_argument("--" + option, type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="og-ai-adapters-") as directory:
        instance.RUN_DIR = directory
        for index, (name, mod) in enumerate([("overhaul", args.overhaul), ("dynamic", args.dynamic)]):
            host = instance.Instance(name, index=410 + index, mods=[str(mod.resolve()), str(args.smarter.resolve())],
                                     keys={**RENDER_KEYS, "mp_port": str(28486 + index), "difficulty_scaling": "true",
                                           "campaign_ai_scaling": "true", "campaign_ai_scaling_rate": "4",
                                           "player_invincible": "true", "campaign_armor_scaling": "false",
                                           "campaign_weapon_scaling": "false", "tutorials": "false"})
            try:
                host.start()
                _code(host, f'LobbyBeginHost();LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
                      'LobbySetModeSetting("challenge_time",0);LobbySetModeSetting("challenge_performance",1);'
                      'LobbySetModeSetting("challenge_performance_rate",4);LobbySetModeSetting("progression",1);'
                      'LobbySetModeSetting("progression_gain_damage",1);LobbySetModeSetting("progression_gain_kill",1);LobbyStart();')
                _ready(host, FIRST)
                _skip(host)
                verify(host, name)
                print(name + " and Smarter Deaths passed")
            finally:
                host.stop()


if __name__ == "__main__":
    main()
