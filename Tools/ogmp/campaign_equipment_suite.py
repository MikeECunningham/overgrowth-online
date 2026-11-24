"""Real armor meshes, localized hits, loot and host handoff on White Flags.

Uses the real first defeat objective and its generated reinforcements. Scripted
challenge wins establish a late-game pressure without hours of waiting; all
equipment is created by the production director and replicated normally.
"""
import math
import time
import traceback

from .campaign_scaling_suite import CAMPAIGN, RENDER_KEYS
from .newcampaign_local_suite import FIRST, _ready, _skip
from .scenarios import script_errors


def campaign_equipment(ctx, fit_only=False):
    host = ctx.instance("equipment_host", width=960, height=540,
                        keys={**RENDER_KEYS, "mp_port": "28104", "tutorials": "false",
                              "auto_ledge_grab": "false", "player_invincible": "true",
                              "difficulty_preset": "Expert", "difficulty_preset_value": "3",
                              "game_difficulty": "1", "difficulty_scaling": "true",
                              "campaign_mp_scaling": "true", "campaign_ai_scaling": "false",
                              "campaign_armor_scaling": "true", "campaign_weapon_scaling": "true",
                              "armor_localized": "true"})
    guest = host if fit_only else ctx.instance("equipment_guest", width=640, height=360,
                         keys={**RENDER_KEYS, "mp_port": "28105", "player_invincible": "true"})
    machines = (host,) if fit_only else (host, guest)
    authority = host
    metrics = {}

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def status(inst=None):
        return (inst or authority).bridge.status()

    def world(inst=None):
        return (inst or authority).bridge.call("state")

    def chars(inst=None):
        return world(inst)["characters"]

    def wait(predicate, label, seconds=35):
        return authority.bridge.wait(predicate, seconds, step=0.12, what=label)

    def probe(expression, inst=None, target="level"):
        inst = inst or authority
        inst.bridge.call("level_messages", prefix="equipment_probe")
        statement = 'level.SendMessage("equipment_probe "+(' + expression + '));'
        code(inst, statement, target)
        messages = inst.bridge.call("level_messages", prefix="equipment_probe")["messages"]
        assert len(messages) == 1, messages
        return messages[0].split(" ", 1)[1]

    def progress():
        return int(hotspot_value("progress"))

    def hotspot_value(name):
        authority.bridge.call("level_messages", prefix="equipment_hotspot")
        code(authority, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
             'if(h.GetID()==14)level.SendMessage("equipment_hotspot "+h.GetIntVar("'+name+'"));}')
        messages = authority.bridge.call("level_messages", prefix="equipment_hotspot")["messages"]
        assert len(messages) == 1, messages
        return messages[0].split(" ", 1)[1]

    def drain(label):
        end, quiet = time.monotonic()+30, None
        while time.monotonic() < end:
            if status()["coop_cutscene"] or status()["dialogue_camera_control"] or int(probe('dialogue_queue.length()')):
                quiet = None
                # A lobby handoff changes the server, while the authored main
                # character keeps its original input owner. Drive normal skip
                # on each local human, allowing that owner to advance dialogue.
                for inst in machines:
                    inst.bridge.call("input",controller=0,hold={"skip_dialogue":1},steps=10)
                time.sleep(0.45)
            else:
                quiet = quiet or time.monotonic()
                if time.monotonic()-quiet > 0.6:
                    return
            time.sleep(0.1)
        raise AssertionError("dialogue did not finish: "+label)

    def hold_party():
        for inst in machines:
            code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                 'if(Online_IsPlayerCharacter(c.GetID())&&!c.network_proxy)c.static_char=true;}')

    def quiet_enemies():
        code(authority, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(!Online_IsPlayerCharacter(c.GetID()))c.Execute("combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");}')

    def near():
        code(authority, 'MovementObject@ enemy=ReadCharacterID(363);MovementObject@ hero=ReadCharacterID(0);'
             'hero.position=Online_SafeSpotNear(enemy.position,enemy.GetFacing(),12);'
             'hero.static_char=true;hero.Execute("FixDiscontinuity();SetOnGround(true);");')
        quiet_enemies()

    def extras(inst=None):
        return [c for c in chars(inst) if c.get("campaign_scaling_source") == 363]

    def added(actor):
        return [p for p in actor.get("armor_attachments", []) if p["region"] > 0]

    def signature(inst=None):
        return {c["common_id"]: (c["combat_stats"].get("campaign_armor_regions", 0),
                tuple(sorted(p["common_id"] for p in added(c)))) for c in extras(inst)}

    def armor_ready():
        local, remote = signature(host), signature(guest)
        if len(local) != 3 or local != remote:
            return False
        return sum(mask.bit_count() for mask, _ in local.values()) >= 8 and all(
            mask.bit_count() == len(pieces) for mask, pieces in local.values())

    def slots(inst, actor):
        state = world(inst)
        ids = {n for n in actor["weapon_slots_common"][:4] if n >= 0}
        return [i for i in state["items"] if i["common_id"] in ids]

    def inspect_equipment(label):
        wait(armor_ready, label+" armor pieces and masks replicate")
        # Finish the bounded creation queue before comparing exact piece sets.
        previous, stable, end = None, None, time.monotonic()+18
        while time.monotonic() < end:
            current = signature(host)
            stable = stable if current == previous else time.monotonic()
            previous = current
            if time.monotonic()-stable > 1.5 and current == signature(guest):
                break
            time.sleep(0.15)
        else:
            raise AssertionError("equipment creation never settled")
        local = {c["common_id"]: c for c in chars(host)}
        remote = {c["common_id"]: c for c in chars(guest)}
        result = []
        for common in signature(host):
            actor, replica = local[common], remote[common]
            assert not actor["combat_stats"]["metal_armor"], "clone inherited source's invisible whole-body armor"
            assert not replica["combat_stats"]["metal_armor"] and not replica["authored_armor_regions"]
            assert not [p for p in replica["armor_attachments"] if p["region"] == 0],"guest clone inherited phantom authored meshes"
            assert not replica["item_connections"],"guest clone inherited an editor starting loadout"
            assert len(slots(host, actor)) <= 1, actor
            assert all(not any(i["common_id"] == connection["common_id"] for connection in actor["item_connections"])
                       for i in slots(host, actor)), "generated weapon became an editor starting-loadout attachment"
            lhs = {p["common_id"]: p for p in added(actor)}
            rhs = {p["common_id"]: p for p in added(replica)}
            assert lhs.keys() == rhs.keys()
            for piece_id, piece in lhs.items():
                other = rhs[piece_id]
                assert piece["path"] == other["path"] and piece["region"] == other["region"]
                assert 1 <= piece["material"] <= 4 and piece["material"] == other["material"], (piece,other)
                assert all(math.isclose(a,b,abs_tol=1e-5) for a,b in zip(piece["scale"],other["scale"]))
                assert piece["bones"] == other["bones"], (piece, other)
                assert piece["fit_error"] < 0.15 and other["fit_error"] < 0.15, (piece,other)
            result.append({"id": common, "slot": actor.get("campaign_scaling_slot"),
                           "mask": actor["combat_stats"]["campaign_armor_regions"],
                           "pieces": list(lhs.values()), "weapons": slots(host,actor)})
        assert sum(bool(row["weapons"]) for row in result) >= 2, result
        metrics[label] = result
        return result

    def fit_portraits():
        # Deterministically pick an eligible head+body armor percentile for
        # two visual fixtures; rejected candidates remain quiet friendly NPCs.
        def mix(value):
            value &= 0xffffffff
            value ^= value >> 16
            value = value*0x7feb352d & 0xffffffff
            value ^= value >> 15
            value = value*0x846ca68b & 0xffffffff
            return (value^(value>>16)) & 0xffffffff
        def planned_head(seed):
            party=status()["difficulty_hud"]["party"]
            allies=1.4*(party["humans"]-1)+0.35*party["bots"]
            intensity=1.5+0.1*(allies if allies<=3 else 3+math.sqrt(allies-3))
            log=math.log1p((intensity-0.3)**3)
            maturity=log/(0.65+log)
            roll=lambda stream:mix(seed^mix(stream+1))/4294967296
            if roll(0)>=maturity:
                return False
            first=mix(seed^0x9e3779b9)%9
            mask=sum(1<<i for i in range(9) if i==first or roll(10+i)<maturity)
            return bool(mask&4 and mask&3)
        hero=next(c for c in chars() if c["common_id"]==0)
        for label,path,wanted_scale in (("scale2_rabbit","Data/Objects/IGF_Characters/IGF_GuardActor.xml",2.0),
                                        ("dog","Data/Objects/characters/dogs/dog_soldier_actor.xml",None)):
            chosen=None
            for attempt in range(12):
                local=authority.bridge.call("spawn",path=path,pos=[hero["pos"][0]+3+attempt,hero["pos"][1],hero["pos"][2]],teams=hero["teams"])["object_id"]
                actor=next(c for c in chars() if c["id"]==local)
                code(authority,f'ReadCharacterID({local}).static_char=true;')
                if planned_head(actor["common_id"]):
                    chosen=actor
                    break
            assert chosen,"bounded visual fixture could not find a head-and-body equipment percentile"
            local=chosen["id"]
            relative=1.0 if wanted_scale is None else wanted_scale/float(probe('this_mo.rigged_object().GetCharScale()',target=local))
            code(authority,f'Object@ o=ReadObjectFromID({local});o.GetScriptParams().SetFloat("Character Scale",{relative});'
                 'o.GetScriptParams().SetString("Teams","equipment_fit_enemy");o.GetScriptParams().SetFloat("Movement Speed",0);'
                 f'ReadCharacterID({local}).Execute("SetParameters();combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");'
                 f'ReadCharacterID({local}).static_char=false;'
                 f'ReadCharacterID({local}).position=Online_SafeSpotNear(ReadCharacterID(363).position+vec3(5,0,3),vec3(0,0,1),2);'
                 f'ReadCharacterID({local}).Execute("FixDiscontinuity();SetOnGround(true);");')
            wait(lambda:any(c["common_id"]==chosen["common_id"] and c["combat_stats"].get("campaign_armor_regions",0)&4 for c in chars()),
                 label+" receives its actual director-created helmet")
            time.sleep(1.8)
            chosen=next(c for c in chars() if c["common_id"]==chosen["common_id"])
            code(authority,f'ReadCharacterID({local}).static_char=true;')
            scale=float(probe('this_mo.rigged_object().GetCharScale()',target=local))
            if wanted_scale is not None:
                assert math.isclose(scale,wanted_scale,abs_tol=1e-4)
            facing=[float(probe('this_mo.GetFacing().'+axis,target=local)) for axis in ('x','y','z')]
            position=chosen["pos"]
            head=[float(probe('this_mo.rigged_object().GetAvgIKChainPos("head").'+axis,target=local)) for axis in ("x","y","z")]
            torso=[float(probe('this_mo.rigged_object().GetAvgIKChainPos("torso").'+axis,target=local)) for axis in ("x","y","z")]
            target=[(head[i]+torso[i])*0.5 for i in range(3)]
            for view,direction in (("front",facing),("side",[-facing[2],0,facing[0]])):
                authority.bridge.call("view",pos=[target[i]+direction[i]*2.5*scale+(0.15*scale if i==1 else 0) for i in range(3)],
                                      target=target,fov=45,pause=False,time=status()["game_time"])
                ctx.shot(authority,label+"_"+view)
                authority.bridge.call("view",clear=True)
            metrics[label+"_fit"]={"scale":scale,"actor":chosen,"head":probe('this_mo.rigged_object().GetAvgIKChainPos("head")',target=local)}
            # The next portrait uses this same open patch. Remove the finished
            # disposable model so its larger silhouette cannot hide the dog.
            if label=="scale2_rabbit":
                code(authority,f'DeleteObjectID({local});')
                time.sleep(0.8)

    try:
        code(host, 'LobbyBeginHost();for(int i=0;i<7;++i)LobbyAddBot();'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
             'LobbySetModeSetting("challenge_performance",1);LobbySetModeSetting("challenge_performance_rate",2);', "menu")
        if not fit_only:
            guest.bridge.call("connect", ip="127.0.0.1:28104")
        wait(lambda: len(status(host)["online"]["players"]) == len(status(guest)["online"]["players"]) == (8 if fit_only else 9),
             "two humans and seven allied bots join")
        guest_player = 255 if fit_only else next(p["id"] for p in status(host)["online"]["players"] if not p["bot"] and p["peer_id"] != 255)
        code(host, 'LobbyStart();', "menu")
        _ready(host,FIRST)
        if not fit_only:
            _ready(guest,FIRST)
        # Distinct legitimate event keys seed a late-game pressure, capped1.5.
        code(host, 'for(int i=0;i<40;++i){string key="equipment_fixture_"+i;'
             'Online_ChallengeEncounterBegin(key);Online_ChallengeEncounterWin(key);}Online_CaptureEnemyDifficulty();')
        wait(lambda: status(host)["difficulty_hud"]["progression"]["general_pressure"] >= 1.49,
             "production challenge awards establish late-game equipment pressure")
        drain("opening")
        hold_party()
        for number, trigger, expected in ((0,477,1),(1,85,2)):
            code(host, f'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint {number} 0 {trigger}");')
            wait(lambda: progress() == expected, "real authored checkpoint advances")
            drain("checkpoint")
            hold_party()
        # The authored original keeps its scripted armor; clones must not
        # inherit that whole-body flag after their physical meshes are cleared.
        code(host, 'ReadObjectFromID(363).GetScriptParams().SetInt("Wearing Metal Armor",1);'
             'ReadCharacterID(363).Execute("SetParameters();");')
        source_weapons = next(c for c in chars(host) if c["common_id"] == 363)["weapon_slots_common"]
        near()
        wait(lambda: len(extras(host)) == len(extras(guest)) == 3, "real ordinary encounter spawns three extras")
        quiet_enemies()
        first = inspect_equipment("first_wave")
        metrics["nan_after_director_gear"] = host.bridge.call("nan_scan")
        if fit_only:
            fit_portraits()
            assert not script_errors(host),script_errors(host)[:6]
            assert not host.bridge.call("nan_scan")["characters"]
            return {"passed":True,"metrics":metrics,"notes":[]}
        assert next(c for c in chars(host) if c["common_id"] == 363)["weapon_slots_common"] == source_weapons
        # A real authored armored prefab supplies the original physical pieces
        # whose ownership must survive disabling the added-armor factor.
        position = extras(host)[0]["pos"]
        stock_id = host.bridge.call("spawn",path="Data/Objects/lugaru_campaign/turner_armored.xml",
                                    pos=[position[0]+4,position[1]+0.1,position[2]],teams="equipment_stock_enemy")["object_id"]
        code(host,f'ReadObjectFromID({stock_id}).SetPlayer(false);ReadCharacterID({stock_id}).ChangeControlScript("enemycontrol.as");'
             f'ReadCharacterID({stock_id}).controlled=false;ReadCharacterID({stock_id}).static_char=true;')
        wait(lambda: all(any(c["common_id"] == stock_id and c.get("authored_armor_regions",0) > 0 for c in chars(inst)) for inst in machines),
             "stock armored prefab exposes real authored coverage")
        stock_original = {inst:{p["common_id"] for c in chars(inst) if c["common_id"] == stock_id
                                for p in c.get("armor_attachments",[]) if p["region"] == 0} for inst in machines}
        assert all(stock_original.values())
        metrics["nan_after_stock_prefab"] = host.bridge.call("nan_scan")
        for inst in machines:
            local = next(c["id"] for c in chars(inst) if c["common_id"] == stock_id)
            code(inst,f'ReadObjectFromID({local}).SetPlayer(false);ReadCharacterID({local}).ChangeControlScript("enemycontrol.as");'
                 f'ReadCharacterID({local}).controlled=false;ReadCharacterID({local}).static_char=true;')
        portrait = max(extras(host),key=lambda c:c["combat_stats"]["campaign_armor_regions"].bit_count())["pos"]
        for inst in machines:
            inst.bridge.call("view",pos=[portrait[0]+2.2,portrait[1]+0.8,portrait[2]+2.8],
                             target=[portrait[0],portrait[1]+0.2,portrait[2]],fov=40,pause=False,time=status(inst)["game_time"])
            ctx.shot(inst,"physical_armor_close")
            inst.bridge.call("view",clear=True)
        metrics["nan_after_portrait"] = host.bridge.call("nan_scan")

        # Test the same hit query used by actual melee with a single head piece,
        # keeping native director state unchanged between these atomic probes.
        head_actor = next(c for c in extras(host) if c["combat_stats"]["campaign_armor_regions"] & 4)
        old_mask = head_actor["combat_stats"]["campaign_armor_regions"]
        metrics["head_ik_measurement"] = probe('this_mo.position+"|"+this_mo.rigged_object().GetAvgIKChainPos("head")+"|"+'
             'this_mo.rigged_object().skeleton().IKBoneStart("head")+"|"+'
             'this_mo.rigged_object().skeleton().GetBoneTransform(31).GetTranslationPart()',target=head_actor["id"])
        head_local = {inst:next(c["id"] for c in extras(inst) if c["common_id"] == head_actor["common_id"]) for inst in machines}
        def coverage(inst):
            # The director now reconciles actual worn slots every update.
            # Isolate this geometric query in one script call instead of
            # leaving a synthetic mask for the director to correct between RPCs.
            inst.bridge.call("level_messages",prefix="equipment_coverage")
            code(inst,'int saved=campaign_armor_regions;campaign_armor_regions=4;'
                 'campaign_authored_armor_regions=0;g_wearing_metal_armor=false;'
                 'level.SendMessage("equipment_coverage "+CampaignArmorProtectsAt(this_mo.rigged_object().GetAvgIKChainPos("head"))+"|"+'
                 'CampaignArmorProtectsAt(this_mo.rigged_object().GetAvgIKChainPos("torso")));'
                 'campaign_armor_regions=saved;',head_local[inst])
            messages=inst.bridge.call("level_messages",prefix="equipment_coverage")["messages"]
            assert len(messages)==1,messages
            return messages[0].split(" ",1)[1]
        for inst in machines:
            assert coverage(inst) == "true|false"
        code(host, 'SetConfigValueBool("armor_localized",false);')
        wait(lambda: all(probe('GetConfigValueBool("armor_localized")',inst=i) == "false" for i in machines),
             "host shared checkbox switches both peers to legacy armor")
        assert all(coverage(inst) == "true|true" for inst in machines)
        code(host, 'SetConfigValueBool("armor_localized",true);')
        wait(lambda: all(probe('GetConfigValueBool("armor_localized")',inst=i) == "true" for i in machines),
             "host shared checkbox restores localized armor on both peers")
        assert all(coverage(inst) == "true|false" for inst in machines)
        metrics["nan_before_native_contact"] = host.bridge.call("nan_scan")
        # Launch a real physics blade through the covered head. Its tip
        # contacts before its center reaches the body, exercising the native
        # pre-Stab/stick callback rather than only the proximity-parry path.
        before_health = probe('temp_health+"|"+blood_health',target=head_local[host])
        projectile = host.bridge.call("spawn",path="Data/Items/rabbit_weapons/rabbit_knife.xml",
                                      pos=[head_actor["pos"][0],head_actor["pos"][1]+20,head_actor["pos"][2]])["object_id"]
        code(host, f'ItemObject@ blade=ReadItemID({projectile});vec3 dir=this_mo.GetFacing()*-1.0f;'
             'vec3 head=this_mo.rigged_object().GetAvgIKChainPos("head");'
             'quaternion r;GetRotationBetweenVectors(normalize(blade.GetLineEnd(0)-blade.GetLineStart(0)),dir,r);'
             'mat4 t=Mat4FromQuaternion(r);t.SetTranslationPart(head-dir*1.2f+vec3(0,0.06f,0));'
             'blade.SetPhysicsTransform(t);blade.ActivatePhysics();blade.last_held_char_id_=0;'
             'blade.SetLinearVelocity(dir*20.0f);blade.SetAngularVelocity(vec3(0));blade.SetThrown();',head_local[host])
        wait(lambda: probe(f'ReadItemID({projectile}).CheckThrownSafe()') == "true","real thrown blade reaches armored body",5)
        assert probe('temp_health+"|"+blood_health',target=head_local[host]) == before_health,"covered native thrown contact damaged victim"
        assert probe(f'ReadItemID({projectile}).StuckInWhom()') == "-1","covered native thrown contact embedded blade"
        assert float(probe(f'distance(ReadItemID({projectile}).GetPhysicsPosition(),this_mo.rigged_object().GetAvgIKChainPos("head"))',
                           target=head_local[host])) < 1.0,"projectile stopped somewhere other than the head"
        metrics["nan_after_native_contact"] = host.bridge.call("nan_scan")
        assert not metrics["nan_after_native_contact"]["objects"],("nonfinite object after native contact",metrics["nan_after_native_contact"])
        metrics["native_thrown_head_contact"]={"item":projectile,"health":before_health,"not_stuck":True}
        code(host,f'DeleteObjectID({projectile});')
        # Exercise the real impact handler as well as its geometric query.
        # The covered head deflects without losing blood/health; the exposed
        # torso still takes the same blade's impact damage.
        host.bridge.call("level_messages",prefix="equipment_impact")
        code(host, 'int item=CreateObject("Data/Items/rabbit_weapons/rabbit_knife.xml",true);'
             'int saved_mask=campaign_armor_regions;campaign_armor_regions=4;'
             'ItemObject@ knife=ReadItemID(item);knife.last_held_char_id_=0;'
             'ReadObjectFromID(item).SetTranslation(this_mo.position+vec3(0,20,0));'
             'float hp=temp_health;float blood=blood_health;this_mo.velocity=vec3(0);'
             'knife.SetLinearVelocity(vec3(6,0,0));knife.SetThrown();'
             'HitByItem("metal",this_mo.rigged_object().GetAvgIKChainPos("head"),item,1);'
             'bool covered=temp_health==hp&&blood_health==blood;'
             'knife.SetLinearVelocity(vec3(6,0,0));knife.SetThrown();'
             'HitByItem("metal",this_mo.rigged_object().GetAvgIKChainPos("torso"),item,1);'
             'bool exposed=temp_health<hp||blood_health<blood;temp_health=hp;blood_health=blood;'
             'DeleteObjectID(item);campaign_armor_regions=saved_mask;level.SendMessage("equipment_impact "+covered+"|"+exposed);',head_local[host])
        impact = host.bridge.call("level_messages",prefix="equipment_impact")["messages"]
        assert len(impact) == 1 and impact[0].endswith("true|true"),impact
        metrics["actual_impact_covered_vs_exposed"] = impact
        for inst in machines:
            code(inst,f'campaign_armor_regions={old_mask};',head_local[inst])
        metrics["localized_vs_legacy"] = ["head only", "whole body", "head only"]

        # Take a director-created dagger. Disabling scaling must never delete
        # player-held loot, nor turn it into an editor loadout restored on Retry.
        armed = next(c for c in extras(host) if slots(host,c))
        loot = slots(host,armed)[0]
        code(host,'DropWeapon();',armed["id"])
        code(host,f'AttachWeapon({loot["id"]});',0)
        wait(lambda: any(i["common_id"] == loot["common_id"] and i["held_by"] == 0 for i in world(host)["items"]),
             "human takes generated weapon")
        code(host,'SetConfigValueBool("campaign_armor_scaling",false);SetConfigValueBool("campaign_weapon_scaling",false);Online_CaptureEnemyDifficulty();')
        wait(lambda: all(c["combat_stats"].get("campaign_armor_regions",0) == 0 and not added(c)
                         for inst in machines for c in extras(inst)), "armor Off removes owned meshes and protection on both peers")
        assert any(i["common_id"] == loot["common_id"] and i["held_by"] == 0 for i in world(host)["items"])
        assert next(c for c in chars(host) if c["common_id"] == 363)["combat_stats"]["metal_armor"]
        for inst in machines:
            stock = next(c for c in chars(inst) if c["common_id"] == stock_id)
            assert {p["common_id"] for p in stock["armor_attachments"] if p["region"] == 0} == stock_original[inst]
        metrics["preserved_authored_pieces"] = {inst.name:sorted(ids) for inst,ids in stock_original.items()}
        metrics["preserved_human_loot"] = loot["common_id"]
        code(host,'SetConfigValueBool("campaign_armor_scaling",true);SetConfigValueBool("campaign_weapon_scaling",true);Online_CaptureEnemyDifficulty();')
        wait(armor_ready,"same frozen armor plans restore after checkbox reenable")

        # Pressure/party changes replace the persistent stats message. Joining
        # afterwards must still install the currently equipped armor mask.
        guest.bridge.call("disconnect")
        wait(lambda: len(status(host)["online"]["players"]) == 8 and not status(guest)["online"]["active"],
             "guest leaves before the late-join equipment replay")
        time.sleep(0.8)  # allow lower-party factor message to coalesce
        guest.bridge.call("connect",ip="127.0.0.1:28104")
        _ready(guest,FIRST)
        wait(lambda: len(status(host)["online"]["players"]) == 9,"guest rejoins existing encounter")
        wait(armor_ready,"late join receives meshes AND latest armor mask after a stats change")
        guest_player = next(p["id"] for p in status(host)["online"]["players"] if not p["bot"] and p["peer_id"] != 255)
        metrics["late_join_after_factor_change"] = signature(guest)

        # Retry replaces the old attempt, retaining exactly one logical wave.
        old_epochs = {(c.get("campaign_scaling_slot"),c.get("campaign_scaling_epoch")) for c in extras(host)}
        code(authority,'level.SendMessage("reset");')
        drain("retry")
        hold_party()
        near()
        wait(lambda: len(extras(host)) == len(extras(guest)) == 3 and
             not old_epochs.intersection((c.get("campaign_scaling_slot"),c.get("campaign_scaling_epoch")) for c in extras()),
             "Retry replaces the old reinforcement actors without accumulating waves",40)
        quiet_enemies()
        final = inspect_equipment("retry_wave")
        assert sorted(row["mask"] for row in final) == sorted(row["mask"] for row in first), "retry rerolled deterministic armor"
        # A real wolf reinforcement is never handed a weapon, even at maximum
        # current progression with weapons-for-extra-enemies enabled.
        position = next(c for c in chars() if c["common_id"] == 363)["pos"]
        wolf_id = authority.bridge.call("spawn",path="Data/Objects/IGF_Characters/IGF_WolfActor.xml",
                                        pos=[position[0]+8,position[1],position[2]],teams="equipment_wolf")["object_id"]
        code(authority,f'Object@ o=ReadObjectFromID({wolf_id});o.GetScriptParams().SetInt("Campaign Boss",1);'
             f'MovementObject@ wolf=ReadCharacterID({wolf_id});wolf.position=Online_SafeSpotNear(ReadCharacterID(363).position,wolf.GetFacing(),8);'
             'wolf.Execute("combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");'
             'SetConfigValueBool("campaign_armor_scaling",false);SetConfigValueBool("campaign_weapon_scaling",true);Online_CaptureEnemyDifficulty();')
        wolf_common = next(c["common_id"] for c in chars() if c["id"] == wolf_id)
        code(authority,'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);if(h.GetID()==14){'
             'int goal=h.GetIntVar("progress")+1;ScriptParams@ p=ReadObjectFromID(14).GetScriptParams();'
             f'p.SetString("goal_"+goal,"defeat {wolf_id}");'
             'p.SetString("goal_"+goal+"_pre","");p.SetString("goal_"+goal+"_post","");'
             'h.SetIntVar("progress",goal);h.SetFloatVar("win_time",-1);}}')
        def wolves(inst):
            return [c for c in chars(inst) if c.get("campaign_scaling_source") == wolf_common]
        wait(lambda: len(wolves(host)) == len(wolves(guest)) == 1,"wolf boss receives one same-species bodyguard")
        quiet_enemies()
        time.sleep(1.2)
        for inst in machines:
            wolf = wolves(inst)[0]
            assert wolf["char_path"].endswith("wolf.xml") and not slots(inst,wolf),wolf
        metrics["unarmed_wolf_bodyguard"] = wolves(authority)

        # Network-host transfer is supported in the lobby, not a loaded world.
        # End game must remove the old wave; the next host starts one new wave
        # with the same roster and shared armor behavior preference.
        code(host,'SetConfigValueBool("campaign_armor_scaling",true);SetConfigValueBool("armor_localized",false);LobbyEndGame();')
        wait(lambda: all(status(i)["online"]["lobby_waiting"] and not status(i)["level_loaded"] for i in machines),
             "End game returns both peers to the persistent lobby",60)
        from pathlib import Path
        code(host,f'SetConfigValueBool("test_equipment_handoff",LobbyPromoteHost({guest_player}));SaveConfig();',"menu")
        values=dict(line.split(": ",1) for line in (Path(host.writedir)/"Data/config.txt").read_text().splitlines() if ": " in line)
        assert values["test_equipment_handoff"] in ("true","1"),"lobby host transfer was rejected"
        wait(lambda: all(not status(i)["online"]["migration_busy"] and status(i)["online"]["decision_host"] == guest_player for i in machines)
             and status(guest)["online"]["hosting"],"remote peer becomes the actual lobby network host",80)
        authority=guest
        assert len(status()["online"]["players"]) == 9
        code(authority,'SetConfigValueBool("test_equipment_legacy",GetConfigValueBool("armor_localized"));SaveConfig();',"menu")
        values=dict(line.split(": ",1) for line in (Path(authority.writedir)/"Data/config.txt").read_text().splitlines() if ": " in line)
        assert values["test_equipment_legacy"] in ("false","0"),"host handoff lost the armor coverage setting"
        metrics["lobby_handoff_roster"]=status()["online"]["players"]
        code(authority,'LobbyStart();',"menu")
        _ready(authority,FIRST)
        _ready(host,FIRST)
        code(authority,'for(int i=0;i<40;++i){string key="equipment_new_host_"+i;Online_ChallengeEncounterBegin(key);Online_ChallengeEncounterWin(key);}Online_CaptureEnemyDifficulty();')
        drain("new host match opening")
        hold_party()
        for number,trigger,expected in ((0,477,1),(1,85,2)):
            code(authority,f'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint {number} 0 {trigger}");')
            wait(lambda:progress()==expected,"new host authored checkpoint advances")
            drain("new host checkpoint")
            hold_party()
        near()
        wait(lambda:len(extras(host))==len(extras(guest))==3,"new host starts exactly one reinforcement wave")
        quiet_enemies()
        inspect_equipment("new_host_wave")

        fit_portraits()
        for inst in machines:
            assert not script_errors(inst), (inst.name,script_errors(inst)[:6])
            assert not inst.bridge.call("nan_scan").get("characters"),inst.name
        return {"passed":True,"metrics":metrics,"notes":[]}
    except Exception:
        metrics["traceback"] = traceback.format_exc()
        for inst in machines:
            try:
                metrics[inst.name+"_failure"]={"status":status(inst),"state":world(inst)}
            except Exception:
                pass
        raise
    finally:
        import json
        from pathlib import Path
        Path(ctx.out_dir,"equipment_details.json").write_text(json.dumps(metrics,indent=2))
        for inst in machines:
            inst.stop()
