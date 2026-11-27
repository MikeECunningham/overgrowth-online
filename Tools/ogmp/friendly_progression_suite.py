"""Check authoritative XP from ordinary character KO/life callbacks and both shop policies."""
import json
import struct
import time
import zlib
from pathlib import Path

from .mode_settings_suite import _code, _rows, _probe, ARENA, ARENA_MAP
from .newcampaign_local_suite import CAMPAIGN, FIRST, SECOND, _ready, _skip
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
    def check_xp_options(arena):
        _code(h, 'array<LobbyOption>@ opts=LobbyModOptions();string checks;'
                 'for(uint i=0;i<opts.length();++i){array<string>@ choices=opts[i].Choices();'
                 'if(choices.length()==2 && choices[0]=="Off" && choices[1]=="On") checks+=opts[i].key+" ";}'
                 'SetConfigValueString("test_xp_checks",checks);')
        checks = set(_probe(h, 'GetConfigValueString("test_xp_checks")').split())
        common = {"shop", "free_buffs", "gain_damage", "gain_kill", "loss_damage", "loss_death", "local_handicaps"}
        mode_options = {"gain_checkpoint", "gain_round", "gain_match"}
        expected = common | ({"gain_round", "gain_match"} if arena else {"gain_checkpoint"})
        assert {key for key in common | mode_options if "progression_" + key in checks} == expected, checks
    def join_campaign(shared):
        _code(h, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);')
        for key, value in (("progression",1),("progression_shared",shared),("progression_timer",0),
                           ("progression_free_buffs",int(not shared))):
            assert _probe(h,f'LobbySetModeSetting("{key}",{value})') == 'true'
        check_xp_options(arena=False)
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
    for _ in range(4):
        kill()
    _code(h,'ProgressionBeginShop(false);',"level")
    wait(lambda: view(g,p2).get("shop_open"),"two independent shops")
    assert wallet(g,p2)==(5*xp,0)
    grant = data(g,p2)["ledger"]
    assert not act(h,p2,'buy','health'),"host spent for a remote seat"
    assert act(g,p2,'buy','health')
    def rank(inst,p,r):
        return next((a["health"] for a in data(inst,p)["ledger"]["players"] if a["id"]==p),0)==r
    wait(lambda: rank(h,p2,1) and rank(g,p2,1),"guest purchases exactly one personal stat")
    assert rank(h,p1,0)

    guest_avatars = [(peer, avatar(peer, p2)) for peer in (h, g)]
    def injure_guest(health=.5):
        _code(g, f"permanent_health={health}f;temp_health={health}f;blood_health=.9f;block_health=1.0f;", guest_avatars[1][1])
    def guest_vitals_match(predicate):
        for peer, actor in guest_avatars:
            char = next(c for c in peer.bridge.state() if c["id"] == actor)
            if not all(predicate(char[key]) for key in ("temp_health", "blood_health", "permanent_health")):
                return False
        return True
    def restore_card(peer):
        return next((c for c in view(peer,p2)["cards"] if c["id"] == "restore_health"), None)
    def quoted(peer, cost):
        card = restore_card(peer)
        return card and card["full_cost"] == 10 and card["cost"] == cost and card["available"] == (cost > 0)

    expected_held = wallet(g,p2)[0]
    assert data(h,p2)["ledger"]["completed_rounds"] == 1
    for health, price in ((.8, 2), (.3, 7)):
        injure_guest(health)
        wait(lambda: all(quoted(peer, price) for peer in (h, g)), "host and guest quote the guest's current injury")
        assert act(g,p2,"buy","restore_health")
        assert act(g,p2,"buy","restore_health")  # a second click before the owner receives the heal
        wait(lambda: guest_vitals_match(lambda hp: hp >= .999) and all(quoted(peer, 0) for peer in (h, g)),
             "the guest and its proxy have fully recovered")
        expected_held -= price
        expected_wallet = (expected_held, 0)
        wait(lambda: wallet(h,p2) == wallet(g,p2) == expected_wallet, "both wallets reflect the confirmed heal")

    # Delay the host's outgoing heal after it accepts a valid purchase. The
    # injured owner's knockout in transit must leave the original balance intact.
    injure_guest()
    wait(lambda: all(quoted(peer, 5) for peer in (h, g)), "injured guest before delayed purchase")
    h.bridge.call("net_sim", latency_ms=800)
    assert act(g,p2,"buy","restore_health")
    wait(lambda: not restore_card(h), "host reserves the purchase")
    assert not guest_vitals_match(lambda hp: hp >= .999)
    _code(g, "SetKnockedOut(_unconscious);", guest_avatars[1][1])
    wait(lambda: next(c for c in h.bridge.state() if c["id"] == guest_avatars[0][1])["knocked_out"] != 0,
         "host receives the owner's knockout")
    time.sleep(1.0)
    assert g.bridge.me()["knocked_out"] != 0 and wallet(h,p2) == expected_wallet
    h.bridge.call("net_sim", latency_ms=0)
    _code(g, "Recover();", guest_avatars[1][1])
    injure_guest()
    wait(lambda: all(quoted(peer, 5) for peer in (h, g)), "the failed purchase releases the reservation")
    assert wallet(h,p2) == wallet(g,p2) == expected_wallet
    assert act(h,p1,'ready') and act(g,p2,'ready')
    wait(lambda: not view(h,p1).get("shop_open"),"both ready end shop")
    injure_guest()
    wait(lambda: guest_vitals_match(lambda hp: hp < .99), "guest wound replicated before checkpoint rolls")
    before_free = wallet(g,p2)
    # Exercise real weighted draws. Drain the guest's preceding hands to reach the
    # first one containing the consumable, including any from authored checkpoints.
    def claim_free_restore():
        for batch in range(16):
            _code(h, f"for(int i=1;i<=16;++i) ProgressionCheckpoint({batch * 16}+i,9000);", "level")
            expected = data(h,p2)["ledger"]["reward_generation"]
            wait(lambda: data(g,p2)["ledger"]["reward_generation"] == expected, "guest receives checkpoint draws")
            hand = view(g,p2)
            while hand["reward_pending"]:
                keys = [c["id"] for c in hand["reward_cards"]]
                choice = "restore_health" if "restore_health" in keys else keys[0]
                token = hand["reward_generation"]
                assert data(g,p2,action="reward",key=choice,generation=token)["submitted"]
                wait(lambda: view(g,p2)["reward_generation"] != token, "exactly one guest hand consumed")
                if choice == "restore_health":
                    return
                hand = view(g,p2)
        raise AssertionError("no restore in 256 injured guest checkpoint rolls")
    claim_free_restore()
    wait(lambda: guest_vitals_match(lambda hp: hp >= .999), "a free restore heals the remote owner and proxy")
    assert wallet(h,p2) == wallet(g,p2) == before_free
    end()
    join_campaign(1)
    kill();kill()
    _code(h,'ProgressionBeginShop(false);',"level")
    wait(lambda: view(g,p2).get("shop_open"),"shared store replicated")
    leader=view(h,p1)["team_leader"]
    new_leader = p2 if leader==p1 else p1
    old_machine,new_machine=(h,g) if leader==p1 else (g,h)
    before=wallet(new_machine,new_leader)
    act(new_machine,new_leader,'buy','bone_damage')
    time.sleep(.3)
    assert wallet(old_machine,leader)==before,"unauthorized team member spent XP"
    assert act(old_machine,leader,'leader',target=new_leader)
    wait(lambda: view(new_machine,new_leader)["team_leader"]==new_leader,"logical team leadership transferred")
    assert act(new_machine,new_leader,'buy','bone_damage')
    wait(lambda: wallet(old_machine,leader)[0]==before[0]-16,"team leader debits shared wallet")
    assert any(t["bone_damage"]==1 for t in data(h,p1)["ledger"]["teams"])
    # The leader pays the injured teammate's quote from the team wallet.
    other_actor = avatar(old_machine, leader)
    _code(old_machine, "permanent_health=.5f;temp_health=.5f;blood_health=1;block_health=1;", other_actor)
    def shared_recovery():
        return next((c for c in view(new_machine,new_leader)["cards"] if c["id"] == "restore_health"), {})
    price = data(h,p1)["ledger"]["completed_rounds"] + 4
    wait(lambda: shared_recovery().get("cost") == price, "shared price uses the recipient's injuries")
    assert act(new_machine,new_leader,'buy','restore_health')
    wait(lambda: wallet(h,p1)[0] == before[0]-16-price and shared_recovery().get("cost") == 0,
         "shared wallet pays once for its injured fighter")
    assert next(c for c in old_machine.bridge.state() if c["id"]==other_actor)["permanent_health"] == 1
    assert act(h,p1,'ready') and act(g,p2,'ready')
    wait(lambda: not view(h,p1).get("shop_open"),"shared shop advances")
    end()
    _code(h,f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
    for key,value in (("progression",1),("progression_gain_damage",1),("progression_loss_damage",1),
                      ("progression_gain_kill",0),("progression_timer",1)):
        assert _probe(h,f'LobbySetModeSetting("{key}",{value})')=='true'
    check_xp_options(arena=True)
    assert _probe(h, 'LobbyGetModeSetting("progression_shop")') == "1"
    assert _probe(h, 'LobbyGetModeSetting("progression_free_buffs")') == "0"
    assert _probe(h, 'LobbySetModeSetting("progression_shop",0) && LobbySetModeSetting("progression_free_buffs",1)') == "true"
    wait(lambda: _probe(g, 'LobbyGetModeSetting("progression_shop") == 0 && LobbyGetModeSetting("progression_free_buffs") == 1') == "true",
         "guest sees free upgrades enabled independently of the paid shop")
    assert _probe(g, 'LobbySetModeSetting("progression_shop",1)') == "false"
    assert _probe(h, 'LobbySetModeSetting("progression_shop",1) && LobbySetModeSetting("progression_free_buffs",0)') == "true"
    wait(lambda: _probe(g,'LobbyGetModeSetting("progression_gain_damage")') == "1", "host XP settings reach guest")
    assert _probe(g,'LobbySetModeSetting("progression_loss_death",1)') == "false"
    _code(h,'LobbyStart();')
    for inst in (h,g): _ready(inst)
    # The actual owner takes final, post-shield damage on a remote character.
    for inst,player,team in ((h,p1,'ledger_red'),(g,p2,'ledger_blue')):
        _code(inst,f'ReadObjectFromID({avatar(inst,player)}).GetScriptParams().SetString("Teams","{team}");',"level")
        _code(inst,'this_mo.static_char=true;invincible=false;ko_shield=0;temp_health=1;blood_health=1;permanent_health=1;',avatar(inst,player))
    held,before=wallet(h,p1)
    _code(g,f'attacked_by_id={avatar(g,p1)};ko_shield=1;TakeDamage(0.2f);',avatar(g,p2))
    time.sleep(.3)
    assert wallet(h,p1)==(held,before)
    _code(g,'ko_shield=0;TakeDamage(0.2f);',avatar(g,p2))
    wait(lambda: wallet(h,p1)[1]>before,"actual remote victim's post-shield damage awards pooled Arena XP")
    earned=wallet(h,p1)[1]-before
    before_bleed=wallet(h,p1)[1]
    _code(g,f'attacked_by_id={avatar(g,p1)};TakeDelayedBloodDamage(.2f);attacked_by_id=this_mo.GetID();'
            'Timestep ts(time_step,1);for(int i=0;i<150 && blood_damage>0;++i) RegenerateHealth(ts);',avatar(g,p2))
    wait(lambda: wallet(h,p1)[1]>=before_bleed+earned-.001,
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


def progression_handicaps(ctx):
    host = ctx.instance("handicap_host", keys={**RENDER_KEYS, "mp_port": "29261"})
    guest = ctx.instance("handicap_guest", width=900, height=720, keys={**RENDER_KEYS, "mp_port": "29262"})

    def wait(condition, label):
        return host.bridge.wait(condition, 45, step=.1, what=label)

    def toggle(peer, seat, enabled=True):
        return _probe(peer, f'LobbySetPlayerHandicap({seat},{str(enabled).lower()})') == 'true'

    def flagged(peer, seat):
        ledger = peer.bridge.call("progression", actor=seat)["ledger"]
        return any(p["id"] == seat and p.get("handicap", False) for p in ledger["players"])

    def press(key):
        for down in (True, False):
            guest.bridge.call("key", key=key, down=down)
            time.sleep(.08)

    def focus_handicap(seat):
        # Use actual keyboard navigation and identify the rendered widget by
        # its ImGui window, slot and label, as in lobby_role_ui.
        for direction in ("Down", "Up", "Down"):
            for _ in range(16):
                state = guest.bridge.status()
                slot = zlib.crc32(struct.pack("<i", seat), state["ui_nav_window_id"])
                if state["ui_nav_id"] == zlib.crc32(b"XP handicap", slot):
                    return
                press(direction)
        raise AssertionError(f"Could not focus the guest's slot {seat} XP handicap checkbox")

    def wallet(peer, seat):
        v = peer.bridge.call("progression", actor=seat)["view"]
        return v["held"], v["pooled"]

    def launch():
        assert _probe(host, "LobbyStart()") == "true"
        for peer in host, guest:
            _ready(peer, FIRST)
        _skip(host)
        wait(lambda: all(p["avatar_id"] >= 0 and p["avatar_id"] != 4294967295 for p in _rows(guest)),
             "all three player avatars deployed")
        for peer in host, guest:
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ p=ReadCharacter(i);'
                        'if(Online_IsPlayerCharacter(p.GetID())&&!p.network_proxy)p.static_char=true;}', "level")

    def character(seat, commands):
        avatar = next(p["avatar_id"] for p in _rows(guest) if p["id"] == seat)
        _code(guest, commands, avatar)

    _code(host, "LobbyBeginHost();")
    _code(guest, "LobbyAddLocalPlayer();")
    guest.bridge.call("connect", ip="127.0.0.1:29261")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 3, "host and two guest-local humans")
    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);')
    assert _probe(host, 'LobbySetModeSetting("progression",1)') == 'true'
    rows = _rows(host)
    host_id = next(p["id"] for p in rows if p["host"])
    first, second = [p["id"] for p in sorted(rows, key=lambda p: p["local_index"]) if p["peer_id"] != 255]
    wait(lambda: _probe(guest, 'LobbyGetModeSetting("progression")') == "1", "progression available on guest")
    assert _probe(host, "LobbyAddSlot()") == "true"
    bot = next(p["id"] for p in _rows(host) if p["bot"])
    assert not toggle(host, bot)
    assert _probe(host, f"LobbySetSlotPlayer({bot},true)") == "true"
    assert toggle(host, bot)
    assert _probe(host, f"LobbyRemovePlayer({bot})") == "true"
    assert not flagged(host, bot) and not toggle(host, bot)
    assert not toggle(guest, second)
    assert toggle(host, second)
    wait(lambda: flagged(guest, second), "host selects a remote slot")
    assert _probe(host, 'LobbySetModeSetting("progression_local_handicaps",1)') == "true"
    wait(lambda: _probe(guest, f"LobbyCanSetPlayerHandicap({second})") == "true", "delegate to guest leader")
    guest.bridge.call("menu", script="multiplayer_lobby.as")
    wait(lambda: any(w["name"] == "Players" for w in guest.bridge.call("ui_state")["windows"]), "guest roster rendered")
    focus_handicap(second)
    ctx.shot(guest, "guest_local_handicap")
    press("Space")
    wait(lambda: not flagged(host, second), "guest leader clears local player's rendered checkbox")
    assert not toggle(guest, host_id)
    _code(guest, "SetControllerPlayer(1);")
    assert not toggle(guest, second)
    _code(guest, "SetControllerPlayer(0);")
    assert _probe(guest, f"LobbyPromoteLeader({second})") == "true"
    wait(lambda: any(p["id"] == second and p["leader"] for p in _rows(guest)), "transfer local authority")
    _code(guest, "SetControllerPlayer(1);")
    assert toggle(guest, second)
    wait(lambda: flagged(host, second), "new guest leader sets their handicap")
    assert _probe(host, 'LobbyGetModeSetting("progression_loss_death")') == "0"
    launch()
    character(second, "attacked_by_id=-1;CutThroat();SetKnockedOut(_dead);")
    wait(lambda: wallet(guest, second)[0] > 0, "defeat credits committed XP with death loss disabled")
    grant, pooled = wallet(guest, second)
    assert pooled == 0 and wallet(host, first) == wallet(host, host_id) == (0, 0)
    character(second, "SetKnockedOut(_dead);")
    attempt = guest.bridge.call("progression", actor=second)["ledger"]["attempt"]
    _code(host, "ProgressionResetAttempt();", "level")
    wait(lambda: guest.bridge.call("progression", actor=second)["ledger"]["attempt"] > attempt, "new attempt replicated")
    assert wallet(guest, second) == (grant, 0)
    character(second, "Recover();attacked_by_id=-1;CutThroat();SetKnockedOut(_dead);")
    wait(lambda: wallet(guest, second) == (2 * grant, 0), "a new defeat earns the same reward once")
    character(second, "Recover();")

    save = Path(host.writedir, "Data/SessionSaves/slot1.json")
    assert _probe(host, "SessionSave(1)", "level") == "true"
    wait(save.exists, "manual save with handicaps")
    saved = json.loads(save.read_text())
    accounts = saved["session_mods"]["og-online-progression"]["runtime"]["players"]
    assert next(p for p in accounts if p["id"] == second)["handicap"]
    assert saved["rules"]["mode_settings"][CAMPAIGN]["progression_local_handicaps"] == 1
    _code(host, "LobbyEndGame();", "level")
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)), "returned to editable lobby")
    assert toggle(host, second, False)
    wait(lambda: not flagged(guest, second), "saved and live handicap now differ")
    assert _probe(host, "SessionLoad(1)") == "true"
    wait(lambda: flagged(host, second) and flagged(guest, second), "disk restores selected slot on both peers")
    assert _probe(host, 'LobbySetModeSetting("progression_loss_death",1)') == "true"
    launch()
    wait(lambda: wallet(guest, second) == (2 * grant, 0), "restored wallet")
    character(second, "Recover();attacked_by_id=-1;CutThroat();")
    wait(lambda: wallet(guest, second) == (3 * grant, 0), "handicap replaces enabled death loss")
    assert _probe(host, "SessionLoad(1)", "level") == "true"
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)), "saved lobby ready for shared upgrades")
    wait(lambda: flagged(host, second), "handicap restored before changing shared settings")
    assert _probe(host, 'LobbySetModeSetting("progression_shared",1)') == "true"
    launch()
    wait(lambda: wallet(guest, second) == (0, 0), "team uses its own wallet")
    character(second, "Recover();attacked_by_id=-1;CutThroat();")
    wait(lambda: wallet(host, host_id) == wallet(guest, second) == (grant, 0),
         "one death reward reaches the shared team wallet")
    assert _probe(host, "SessionLoad(1)", "level") == "true"
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in (host, guest)) and flagged(host, second), "saved lobby reopened for a fresh run")
    assert _probe(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{SECOND}",false)') == "true"
    assert toggle(host, second, False) and toggle(host, first)
    first_index = next(p["local_index"] for p in _rows(host) if p["id"] == first)
    assert _probe(host, f"LobbySwapSlots({first},{second})") == "true"
    wait(lambda: any(p["id"] == second and p["local_index"] == first_index for p in _rows(guest)) and flagged(guest, first),
         "the handicap stays with its slot through reassignment")
    assert _probe(host, "LobbyStart()") == "true"
    for peer in (host, guest):
        _ready(peer, SECOND)
    wait(lambda: all(flagged(p, first) and not flagged(p, second) for p in (host, guest)),
         "starting a different map retains only the selected handicaps")
    assert wallet(guest, second) == (0, 0)
    assert all(not script_errors(peer) for peer in (host, guest))
    return {"passed": True, "metrics": {"committed_xp_per_defeat": grant, "saved_player": second}, "notes": []}
