"""Real sight/hearing queries, shared stealth purchases and opposed choke damage."""
import math

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _rows, _skip
from .scenarios import script_errors


def stealth_stats(ctx):
    host = ctx.instance("stealth_stats_host", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28561", "difficulty_scaling": "true", "game_difficulty": "0.5",
        "campaign_mp_scaling": "false", "campaign_ai_scaling": "false", "campaign_armor_scaling": "false",
        "campaign_weapon_scaling": "false", "player_invincible": "true"})
    guest = ctx.instance("stealth_stats_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "28562"})
    peers = host, guest
    def wait(fn, message):
        return host.bridge.wait(fn, 35, step=.1, what=message)
    def probe(code, expression, target="level"):
        host.bridge.call("level_messages", prefix="stealth_probe ")
        _code(host, code + 'level.SendMessage("stealth_probe " + (' + expression + '));', target)
        values = host.bridge.call("level_messages", prefix="stealth_probe ")["messages"]
        assert values, "stealth probe did not execute"
        return values[-1].split(" ", 1)[1]
    def stats(peer, common):
        return next((c["combat_stats"] for c in peer.bridge.state() if c["common_id"] == common), {})
    def data(peer, player, **kw):
        return peer.bridge.call("progression", actor=player, **kw)

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",1);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:28561")
    wait(lambda: all(len(_rows(p)) == 2 for p in peers), "two human seats")
    _code(host, "LobbyStart();", "menu")
    for p in peers:
        _ready(p, FIRST)
    _skip(host)
    _code(host, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
          'if(Online_IsPlayerCharacter(c.GetID())) c.static_char=true;}')
    seats = _rows(host)
    owner = next(r["id"] for r in seats if r["host"])
    hero = next(r["avatar_id"] for r in seats if r["id"] == owner)
    def spawn(name, z, x=0):
        id = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                              pos=[x,500,z], teams=name)["object_id"]
        _code(host, 'this_mo.static_char=true;g_no_look_around=true;head_look=vec3(0);torso_look=vec3(0);'
              'combat_allowed=false;always_unaware=true;this_mo.SetRotationFromFacing(vec3(0,0,1));', id)
        return id
    observer, near, far = spawn("stealth_observer",0), spawn("stealth_target",15), spawn("stealth_target",22.5,6)
    wait(lambda: all(stats(host,id).get("campaign_base_difficulty") == .5 for id in (observer,near,far)),
         "fixtures initialized")
    # Earn through the authoritative campaign reward/bank path, then buy both
    # upgrades from the shared shop. Every teammate gets the replicated ranks.
    _code(host, f'for(int n=0;n<6;++n){{ProgressionReportHit({near},{hero},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();}ProgressionBeginShop(false);')
    wait(lambda: data(guest,owner)["view"].get("shop_open"), "shared shop replicated")
    leader = data(host,owner)["view"]["team_leader"]
    buyer = host if leader == owner else guest
    cards = {c["id"]: c for c in data(buyer,leader)["view"]["cards"]}
    assert {"health","damage","speed","stealth_passive","stealth_active","luck"} <= cards.keys(), data(buyer,leader)
    assert all(cards[key]["rarity"] == 0 for key in ("health","damage","speed","stealth_passive","stealth_active","luck"))
    assert cards["luck"]["cost"] == cards["health"]["cost"]
    for key in ("stealth_passive","stealth_active","luck"):
        assert data(buyer,leader,action="buy",key=key)["submitted"]
    def installed():
        return all(stats(p,r["avatar_id"]).get("progression_stealth_passive") == 1 and
                   stats(p,r["avatar_id"]).get("progression_stealth_active") == 1
                   for p in peers for r in seats)
    wait(installed, "shared stealth ranks installed on both peers")
    wait(lambda: all(data(p,r["id"])["view"]["stats"]["luck_rank"] == 1 for p in peers for r in seats),
         "shared Luck propagated to both HUDs")
    for p in peers:
        ledger = data(p,owner)["ledger"]
        assert any(t["stealth_passive"] == t["stealth_active"] == t["luck"] == 1 for t in ledger["teams"])
        assert math.isclose(data(p,owner)["view"]["stats"]["reward_chance"], .216)
    for p in peers:
        local = next(r["id"] for r in _rows(p) if (r["peer_id"] == 255) == (p is host))
        assert data(p,local,action="ready",key="")["submitted"]
    wait(lambda: not data(host,owner)["view"].get("shop_open"), "shared shop closes")

    # Exercise the actual FOV hull and line-of-sight query above the map, with
    # static fixture poses and only the candidate temporarily eligible for AI.
    def visible(candidate, passive=0, active=0, crouched=False, vitality=1, damage=1):
        _code(host, f'progression_stealth_passive={passive};progression_stealth_active={active};'
              f'duck_amount={1 if crouched else 0};', candidate)
        return probe(f'SetCampaignScaling({vitality}, {damage}, 1, 0);'
                     'fov_focus=vec3(75,60,20);fov_peripheral=vec3(90,60,5);fov_hulls_calculated=false;'
                     f'MovementObject@ target=ReadCharacterID({candidate});target.static_char=false;'
                     'array<int> seen;GetVisibleCharacters(0,seen);target.static_char=true;',
                     f'seen.find({candidate})>=0', observer) == "true"
    assert visible(near), "ordinary standing target missing from authored sightline"
    assert not visible(near,passive=10), "passive stealth did not shorten sight range"
    assert visible(near,passive=10,crouched=True), "standing rank leaked into crouched stealth"
    assert not visible(near,active=10,crouched=True), "active stealth did not shorten crouched sight range"
    assert visible(near,active=10), "crouched rank leaked into standing stealth"
    assert not visible(far), "baseline sight range unexpectedly reaches distant target"
    assert visible(far,vitality=6,damage=2), "sampled stat scaling did not extend sight range"

    # Sounds use the sender's stance and the listener's sampled hearing range.
    # Foley adds 0.3 suspicion when heard; no awareness timer or combat is used.
    def hears(passive=0,active=0,crouched=False,range=5,vitality=1,damage=1):
        _code(host, f'progression_stealth_passive={passive};progression_stealth_active={active};'
              f'duck_amount={1 if crouched else 0};', near)
        return float(probe('SetCampaignScaling('+str(vitality)+','+str(damage)+',1,0);'
                           'this_mo.static_char=false;goal=_patrol;suspicious_amount=0;awake_time=2;listening=true;'
                           f'ReadCharacterID({near}).Execute("AISound(this_mo.position,{range},_sound_type_foley);");'
                           'this_mo.static_char=true;', 'suspicious_amount', observer)) > .1
    assert hears(range=8), "ordinary target foley was not heard"
    assert not hears(passive=10,range=8), "passive stealth did not shorten hearing range"
    assert hears(passive=10,crouched=True,range=8), "passive stealth affected crouched sound"
    assert not hears(active=10,crouched=True,range=8), "active stealth did not shorten hearing range"
    assert hears(active=10,range=8), "active stealth affected standing sound"
    assert not hears(range=6.5), "baseline hearing unexpectedly reaches distant sound"
    assert hears(range=6.5,vitality=2,damage=1.5), "sampled stat scaling did not extend hearing range"

    # Run the real tethered victim damage path for one quarter second, without
    # relying on animation timing or an AI's choice to resist/release the hold.
    def choke(health,damage):
        _code(host, f'progression_damage={damage};campaign_authored_damage=1;CampaignApplyAuthoredScaling();'
              'for(uint i=0;i<weapon_slots.length();++i) weapon_slots[i]=-1;', near)
        return 1-float(probe(f'progression_health={health};campaign_authored_resistance=1;'
                            'SetCampaignScaling(1,1,1,0);game_difficulty=.5;temp_health=1;permanent_health=1;'
                            'invincible=false;equipped_damage_factor=1;ko_shield=0;'
                            f'tether_id={near};tethered=_TETHERED_REARCHOKED;Timestep step(.25f,1);'
                            'HandleTethering(step);tethered=_TETHERED_FREE;', 'temp_health', observer))
    base, faster, opposed = choke(0,0),choke(0,10),choke(10,10)
    assert math.isclose(base,.125,abs_tol=1e-5),base
    assert math.isclose(faster/base,1.6,abs_tol=1e-4),(base,faster)
    assert math.isclose(opposed/faster,1/1.8,abs_tol=1e-4),(faster,opposed)
    assert all(not script_errors(p) for p in peers), [script_errors(p) for p in peers]
    return {"passed":True,"metrics":{"shared_stealth_rank":1,"shared_luck_rank":1,"reward_chance":.216,
                                      "sight_perception":1.5,"hearing_perception":1.225,
                                      "choke_damage":{"base":base,"damage_10":faster,"health_10_damage_10":opposed}},"notes":[]}
