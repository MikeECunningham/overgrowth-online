"""Measure standing and ragdoll fall damage on the same floor before and after a purchase."""
from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows, _skip
from .scenarios import script_errors


def progression_falls(ctx):
    host = ctx.instance("fall_resistance", keys={**RENDER_KEYS,"mp_port":"29971"})
    _code(host, f'LobbyBeginHost(true);LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_gain_checkpoint",1);LobbyStart();')
    _ready(host, FIRST)
    _skip(host)
    host.bridge.wait(lambda: not host.bridge.status()["coop_cutscene"], 25, what="hero leaves cutscene")
    seat = _rows(host)[0]["id"]
    hero = _rows(host)[0]["avatar_id"]
    _code(host, 'for(int i=0;i<GetNumCharacters();i++) ReadCharacter(i).static_char=true;', "level")
    def stats():
        return host.bridge.call("progression",actor=seat)["view"]["stats"]
    def measure(ragdoll):
        # Run the actual collision callbacks against the same environment
        # contact. Save the result in a script parameter before regeneration.
        # The controlled landing resolves its contact against this map's floor.
        reset = 'RecoverHealth();invincible=false;ko_shield=0;fall_death=false;'
        if ragdoll:
            impact = 'ragdoll_descent_speed=36;Collided(this_mo.position.x,this_mo.position.y,this_mo.position.z,80,1);'
        else:
            impact = ('vec3 start=this_mo.position;on_ground=false;air_time=1;'
                      'this_mo.position.y+=2.0f;for(int step=0;step<100 && permanent_health==1.0f;++step){'
                      'this_mo.velocity=vec3(0,-36,0);last_col_pos=this_mo.position;on_ground=false;'
                      'this_mo.position.y-=0.15f;HandleAirCollisions(Timestep(time_step,1));}'
                      'this_mo.position=start;last_col_pos=start;this_mo.velocity=vec3(0);')
        _code(host, reset+impact+'ReadObjectFromID(this_mo.GetID()).GetScriptParams().SetFloat("Test impact",1-permanent_health);', hero)
        return float(_probe(host,'ReadObjectFromID(this_mo.GetID()).GetScriptParams().GetFloat("Test impact")',hero))
    base = [measure(False), measure(True)]
    assert min(base)>0, base
    _code(host,'ProgressionCheckpoint(1,7001);ProgressionBeginShop(false);',"level")
    assert host.bridge.call("progression",actor=seat,action="buy",key="fall_resistance")["submitted"]
    host.bridge.wait(lambda: _probe(host,'progression_fall_resistance',hero)=='1',20,what="purchased rank on hero")
    assert stats()["fall_resistance_rank"] == 1
    reduction = stats()["fall_resistance"]
    assert 0 < reduction < 1
    improved = [measure(False), measure(True)]
    for normal, protected in zip(base,improved):
        assert abs(protected/normal-(1-reduction)) < .007, (normal,protected,reduction)
    assert float(_probe(host,'LongFallDamageScale(5)',hero)) == 1
    _code(host,'fall_death=true;',hero)
    assert float(_probe(host,'LongFallDamageScale(40)',hero)) == 1
    assert not script_errors(host)
    return {"passed":True,"metrics":{"standing_before_after":[base[0],improved[0]],"ragdoll_before_after":[base[1],improved[1]],
                                     "first_rank_reduction":reduction},"notes":[]}
