"""Nightmare rates, frozen AI strength, authoritative preferences and migration."""
import math
import time

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _rows, _skip
from .mode_settings_suite import _probe
from .scenarios import script_errors


def difficulty_rates(ctx):
    host=ctx.instance("rate_scaling_host",width=1100,height=700,keys={**RENDER_KEYS,"mp_port":"28753",
        "difficulty_scaling":"true","campaign_mp_scaling":"false","campaign_ai_scaling":"true",
        "campaign_ai_scaling_rate":"2","campaign_armor_scaling":"false","campaign_weapon_scaling":"false",
        "tutorials":"false","player_invincible":"true"})
    guest=ctx.instance("rate_scaling_guest",width=640,height=400,keys={**RENDER_KEYS,"mp_port":"28754"})
    peers=(host,guest)
    def wait(fn,label,seconds=35):
        try: return host.bridge.wait(fn,seconds,step=.1,what=label)
        except Exception:
            import json
            from pathlib import Path
            Path(ctx.out_dir,"rate_failure.json").write_text(json.dumps({i.name:{"status":i.bridge.status(),"state":i.bridge.call("state")} for i in peers},indent=2))
            raise
    def ai(inst):
        actor=next((c for c in inst.bridge.state() if c["common_id"]==21),None)
        return actor["combat_stats"].get("campaign_ai_bonus") if actor else None
    def both(value): return all(ai(i) is not None and math.isclose(ai(i),value,abs_tol=1e-6) for i in peers)
    _code(host,'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);',"menu")
    assert _probe(host,'LobbyGetModeSetting("challenge_time")')=='2'
    assert _probe(host,'LobbyGetModeSetting("challenge_round")')=='2'
    assert _probe(host,'LobbyGetModeSetting("challenge_performance")')=='0'
    guest.bridge.call("connect",ip="127.0.0.1:28753")
    wait(lambda:all(len(_rows(i))==2 for i in peers),"rate test roster")
    guest_id=next(p["id"] for p in _rows(host) if not p["host"])
    _code(host,'LobbySetModeSetting("challenge_time",0);LobbySetModeSetting("challenge_round",0);'
          'LobbySetModeSetting("challenge_performance",1);LobbySetModeSetting("challenge_performance_rate",4);LobbyStart();',"menu")
    for i in peers: _ready(i,FIRST)
    _skip(host)
    # Stock NPC21 is one of the deterministic selected AI-variation IDs.
    _code(host,'ReadObjectFromID(21).SetEnabled(true);ReadObjectFromID(21).GetScriptParams().SetString("Teams","rate_fixture_enemy");')
    assert _probe(host,'ReadObjectFromID(21).GetScriptParams().GetString("Teams")',"level")=="rate_fixture_enemy"
    _code(host,'ReadCharacterID(21).position=Online_SafeSpotNear(ReadCharacterID(0).position,ReadCharacterID(0).GetFacing(),3);'
          'ReadCharacterID(21).Execute("SetParameters();invincible=true;combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");'
          'ReadCharacterID(21).static_char=true;')
    _code(host,'for(int n=0;n<40;++n){string k="nightmare_rate_"+n;Online_ChallengeEncounterBegin(k);Online_ChallengeEncounterWin(k);}')
    assert float(_probe(host,'Online_ChallengePressure()',"level"))>=1.5
    _code(host,'Online_CaptureEnemyDifficulty();')
    wait(lambda:both(.03),"Standard AI amount installed on both peers")
    _code(host,'SetConfigValueInt("campaign_ai_scaling_rate",4);')
    wait(lambda:_probe(guest,'GetConfigValueInt("campaign_ai_scaling_rate")',"level")=='4',"Nightmare AI preference replicated")
    _code(guest,'SetConfigValueInt("campaign_ai_scaling_rate",1);')
    assert _probe(guest,'GetConfigValueInt("campaign_ai_scaling_rate")',"level")=='4'
    time.sleep(.5)
    assert both(.03),"editing the rate strengthened a living enemy before a boundary"
    wait(lambda:any("AI Nightmare" in s for s in host.bridge.status()["difficulty_hud"].get("lines",[])),"HUD names configured AI rate")
    snapshot=host.bridge.status()["online"]["challenge"]["enemy_difficulty"]
    assert snapshot["ai_rate"]==2
    _code(host,'Online_CaptureEnemyDifficulty();')
    wait(lambda:both(.06),"next boundary installs doubled Nightmare AI amount")
    _code(host,'SetConfigValueInt("campaign_ai_scaling_rate",5);')
    assert _probe(host,'GetConfigValueInt("campaign_ai_scaling_rate")',"level")=='4'
    _code(host,'SetConfigValueInt("campaign_ai_scaling_rate",0);Online_CaptureEnemyDifficulty();')
    wait(lambda:both(0),"Off removes the optional variation at a boundary")
    _code(host,'SetConfigValueInt("campaign_ai_scaling_rate",4);Online_CaptureEnemyDifficulty();')
    wait(lambda:both(.06),"Nightmare re-enabled")
    _code(host,'LobbyEndGame();')
    wait(lambda:all(i.bridge.status()["online"]["lobby_waiting"] and not i.bridge.status()["level_loaded"] for i in peers),"rate migration lobby")
    # With the scene closed, the next-wave readout uses the reference
    # estimate. Its possible AI maximum must use the chosen rate as well.
    wait(lambda:math.isclose(host.bridge.status()["difficulty_hud"]["next"]["factors"]["ai"]["max"],.06,abs_tol=1e-6),
         "Nightmare reference forecast uses the selected rate")
    assert _probe(host,f'LobbyPromoteHost({guest_id})')=='true'
    wait(lambda:all(not i.bridge.status()["online"]["migration_busy"] and
                   i.bridge.status()["online"]["decision_host"]==guest_id for i in peers),"rate preference host migration",80)
    for i in peers:
        assert _probe(i,'GetConfigValueInt("campaign_ai_scaling_rate")')=='4'
        assert _probe(i,'LobbyGetModeSetting("challenge_performance_rate")')=='4'
        assert not script_errors(i),script_errors(i)
    return {"passed":True,"metrics":{"default_rates":"Standard","performance_default":"Off",
        "standard_ai":.03,"nightmare_ai":.06,"frozen_until_boundary":True,"migration":True},"notes":[]}
