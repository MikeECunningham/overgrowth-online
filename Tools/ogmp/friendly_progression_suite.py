"""Check authoritative XP from ordinary character KO/life callbacks and both shop policies."""
import time

from .mode_settings_suite import _code, _rows, _probe, ARENA, ARENA_MAP
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _skip
from .campaign_scaling_suite import RENDER_KEYS
from .scenarios import script_errors


def friendly_progression(ctx):
    h = ctx.instance("xp_ledger_host", width=800, height=600, keys={**RENDER_KEYS, "mp_port":"28401", "game_difficulty":"0.5", "difficulty_scaling":"false"})
    g = ctx.instance("xp_ledger_guest", width=640, height=480, keys={**RENDER_KEYS, "mp_port":"28402"})
    metrics = {}
    def wait(fn, label, timeout=40):
        return h.bridge.wait(fn, timeout, step=.1, what=label)
    def data(inst, player, **kw):
        return inst.bridge.call("progression", actor=player, **kw)
    def view(inst, player):
        return data(inst,player)["view"]
    def wallet(inst, player):
        v=view(inst,player)
        return tuple(v.get(k,-1) for k in ("held","pooled"))
    def act(inst, player, action, key="", target=None):
        return data(inst,player,action=action,key=key,target=player if target is None else target)["submitted"]
    def join_campaign(shared):
        _code(h, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);')
        for key, value in (("progression",1),("progression_shared",shared),("progression_timer",0)):
            assert _probe(h,f'LobbySetModeSetting("{key}",{value})') == 'true'
        _code(h, 'LobbyStart();')
        for peer in (h,g): _ready(peer,FIRST)
        deadline=time.monotonic()+25
        while time.monotonic()<deadline and (h.bridge.status()["coop_cutscene"] or h.bridge.status()["dialogue_camera_control"]):
            _skip(h)
        for peer in (h,g):
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ p=ReadCharacter(i);'
                        'if(Online_IsPlayerCharacter(p.GetID())&&!p.network_proxy)p.static_char=true;}',"level")
    def end():
        _code(h,"LobbyEndGame();","level")
        wait(lambda: all(i.bridge.status()["online"]["lobby_waiting"] and not i.bridge.status()["level_loaded"] for i in (h,g)),"back in lobby")
    def avatar(inst, id):
        return next(p["avatar_id"] for p in _rows(inst) if p["id"]==id)
    def kill():
        p=next(c["pos"] for c in h.bridge.state() if c["id"]==avatar(h,p1))
        enemy=h.bridge.call("spawn",path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",pos=[p[0]+3,p[1]+.25,p[2]+3],teams="xp_test_enemy")["object_id"]
        _code(h, f'attacked_by_id={avatar(h,p1)};SetKnockedOut(_unconscious);SetKnockedOut(_dead);',enemy)
        return enemy

    _code(h,'LobbyBeginHost();')
    g.bridge.call("connect",ip="127.0.0.1:28401")
    wait(lambda: len(_rows(h))==len(_rows(g))==2,"two seats")
    p1=next(p["id"] for p in _rows(h) if p["peer_id"]==255)
    p2=next(p["id"] for p in _rows(h) if p["peer_id"]!=255)
    join_campaign(0)
    kill()
    wait(lambda: wallet(g,p2)[1]>0,"an allied victim defeat reaches guest's campaign pool")
    xp=wallet(g,p2)[1]
    assert wallet(h,p1)==(0,xp) and wallet(g,p2)==(0,xp)
    _code(h,'ProgressionCheckpoint();',"level")
    wait(lambda: wallet(g,p2)==(xp,0),"checkpoint banks both accounts once")
    _code(g,'SetKnockedOut(_dead);',avatar(g,p2))
    wait(lambda: next(c["knocked_out"] for c in h.bridge.state() if c["id"]==avatar(h,p2))!=0,"guest down observed by host")
    kill()
    wait(lambda: wallet(h,p1)[1]==xp,"surviving host gets subsequent kill")
    assert wallet(g,p2)==(xp,0)
    _code(h,'ProgressionResetAttempt();',"level")
    wait(lambda: wallet(h,p1)==(xp,0),"reset discards pending and preserves held")
    _code(g,'Recover();',avatar(g,p2))
    wait(lambda: next(c["knocked_out"] for c in h.bridge.state() if c["id"]==avatar(h,p2))==0,"guest revived")
    kill()
    _code(h,'ProgressionBeginShop(false);',"level")
    wait(lambda: view(g,p2).get("shop_open"),"two independent shops")
    assert wallet(g,p2)==(2*xp,0)
    grant = data(g,p2)["ledger"]
    assert not act(h,p2,'buy','health'),"host spent for a remote seat"
    assert act(g,p2,'buy','health')
    def rank(inst,p,r):
        return next((a["health"] for a in data(inst,p)["ledger"]["players"] if a["id"]==p),0)==r
    wait(lambda: rank(h,p2,1) and rank(g,p2,1),"guest purchases exactly one personal stat")
    assert rank(h,p1,0)
    assert act(h,p1,'ready') and act(g,p2,'ready')
    wait(lambda: not view(h,p1).get("shop_open"),"both ready end shop")
    end()
    join_campaign(1)
    kill();kill()
    _code(h,'ProgressionBeginShop(false);',"level")
    wait(lambda: view(g,p2).get("shop_open"),"shared store replicated")
    leader=view(h,p1)["team_leader"]
    new_leader = p2 if leader==p1 else p1
    old_machine,new_machine=(h,g) if leader==p1 else (g,h)
    before=wallet(new_machine,new_leader)
    act(new_machine,new_leader,'buy','damage')
    time.sleep(.3)
    assert wallet(old_machine,leader)==before,"unauthorized team member spent XP"
    assert act(old_machine,leader,'leader',target=new_leader)
    wait(lambda: view(new_machine,new_leader)["team_leader"]==new_leader,"logical team leadership transferred")
    assert act(new_machine,new_leader,'buy','damage')
    wait(lambda: wallet(old_machine,leader)[0]==before[0]-16,"team leader debits shared wallet")
    assert any(t["damage"]==1 for t in data(h,p1)["ledger"]["teams"])
    assert act(h,p1,'ready') and act(g,p2,'ready')
    wait(lambda: not view(h,p1).get("shop_open"),"shared shop advances")
    end()
    _code(h,f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
    for key,value in (("progression",1),("progression_combat",1),("progression_timer",1)):
        assert _probe(h,f'LobbySetModeSetting("{key}",{value})')=='true'
    _code(h,'LobbyStart();')
    for inst in (h,g): _ready(inst)
    # The actual owner takes final, post-shield damage on a remote character.
    for inst,player,team in ((h,p1,'ledger_red'),(g,p2,'ledger_blue')):
        _code(inst,f'ReadObjectFromID({avatar(inst,player)}).GetScriptParams().SetString("Teams","{team}");',"level")
        _code(inst,'this_mo.static_char=true;invincible=false;ko_shield=0;temp_health=1;blood_health=1;permanent_health=1;',avatar(inst,player))
    before=wallet(h,p1)[0]
    _code(g,f'attacked_by_id={avatar(g,p1)};ko_shield=1;TakeDamage(0.2f);',avatar(g,p2))
    time.sleep(.3)
    assert wallet(h,p1)[0]==before
    _code(g,'ko_shield=0;TakeDamage(0.2f);',avatar(g,p2))
    wait(lambda: wallet(h,p1)[0]>before,"actual remote victim's post-shield damage awards instant Arena XP")
    earned=wallet(h,p1)[0]-before
    before_bleed=wallet(h,p1)[0]
    _code(g,f'attacked_by_id={avatar(g,p1)};TakeDelayedBloodDamage(.2f);attacked_by_id=this_mo.GetID();'
            'Timestep ts(time_step,1);for(int i=0;i<150 && blood_damage>0;++i) RegenerateHealth(ts);',avatar(g,p2))
    wait(lambda: wallet(h,p1)[0]>=before_bleed+earned-.001,
         "delayed bleeding credits actual blood loss to its original attacker")
    _code(h,'ProgressionBeginShop(false);',"level")
    wait(lambda: view(g,p2).get("shop_open"),"ten-second store opens")
    wait(lambda: not view(g,p2).get("shop_open"),"shop closes without either seat ready",16)
    end()
    saved=data(h,p1)["ledger"]
    assert _probe(h,f'LobbyPromoteHost({p2})')=='true'
    wait(lambda: all(not inst.bridge.status()["online"]["migration_busy"] and
                     inst.bridge.status()["online"]["decision_host"]==p2 for inst in (h,g)),
         "progression survives a real session handoff",70)
    assert g.bridge.status()["online"]["hosting"] and h.bridge.status()["online"]["client"]
    for inst in (h,g):
        migrated=data(inst,p2)["ledger"]
        for key in ("players","teams","shop"):
            assert migrated[key]==saved[key],(key,migrated,saved)
    assert all(not script_errors(i) for i in (h,g))
    metrics.update(campaign_kill_xp=xp,independent_commit=grant["players"],shared_leader=new_leader,combat_damage_xp=earned)
    return {"passed":True,"metrics":metrics,"notes":[]}
