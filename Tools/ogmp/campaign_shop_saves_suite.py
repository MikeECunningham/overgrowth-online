"""Cold resume the resolved chapter destination and the final campaign loop."""
import json
import shutil
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, SECOND, _code, _ready, _rows, _skip
from .scenarios import script_errors


def campaign_shop_saves(ctx):
    def instance(name, port):
        return ctx.instance(name, width=800, height=480, keys={**RENDER_KEYS, "mp_port":str(port),
            "player_invincible":"true", "tutorials":"false", "difficulty_scaling":"true"})

    def wait(inst, fn, label):
        return inst.bridge.wait(fn,45,step=.1,what=label)

    def card(inst, slot):
        return Path(inst.writedir,"Data/SessionSaves",f"slot{slot}.json")

    def record(inst, slot):
        try: return json.loads(card(inst,slot).read_text())
        except (FileNotFoundError,json.JSONDecodeError): return {}

    def view(inst):
        return inst.bridge.call("progression",actor=_rows(inst)[0]["id"])["view"]

    def act(inst, action, key=""):
        assert inst.bridge.call("progression",actor=_rows(inst)[0]["id"],action=action,key=key)["submitted"]

    def cold_resume(source, name, port, slot, level, free_buffs=None):
        fresh=instance(name,port)
        for folder in ("SessionSaves","Textures/SessionSaves"):
            shutil.copytree(Path(source.writedir,"Data",folder),Path(fresh.writedir,"Data",folder),dirs_exist_ok=True)
        _code(fresh,f'SetConfigValueBool("test_shop_resume",SessionLoad({slot}));',"menu")
        wait(fresh,lambda:fresh.bridge.status()["online"]["lobby_waiting"],"saved shop opens its lobby")
        wait(fresh,lambda:fresh.bridge.call("progression")["ledger"]["open"],"saved shop ledger arrives in the lobby")
        if free_buffs is not None:
            _code(fresh, f'LobbySetModeSetting("progression_free_buffs",{int(free_buffs)});', "menu")
        _code(fresh,'LobbyStart();',"menu")
        _ready(fresh,level)
        wait(fresh,lambda:view(fresh).get("shop_open",False) and not fresh.bridge.status()["dialogue_camera_control"],
             "saved shop reopens in the loaded world")
        time.sleep(.5)
        assert level in fresh.bridge.status()["level"],"shop resumed directly into its destination"
        return fresh

    source=instance("campaign_shop_source",28831)
    _code(source,'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_timer",0);'
          'LobbySetModeSetting("challenge_time",0);LobbySetModeSetting("progression_gain_checkpoint",1);LobbyStart();',"menu")
    _ready(source,FIRST);_skip(source)
    _code(source,'ProgressionCheckpoint(1, 9000);')
    _code(source,'SendGlobalMessage("levelwin");')
    wait(source,lambda:view(source).get("shop_open",False),"real chapter completion opens shop")
    shop = view(source)
    assert shop["held"] >= 16 and not {"luck_rank", "reward_chance"}.intersection(shop["stats"])
    assert all(c["id"] != "luck" for c in shop["cards"])
    assert not source.bridge.call("progression",actor=_rows(source)[0]["id"],action="buy",key="luck")["submitted"]
    wait(source,lambda:record(source,0).get("world",{}).get("campaign_transition")=="slaver_camp",
         "shop autosave stores the already resolved next chapter")
    _code(source,'SessionSave(1);')
    wait(source,lambda:record(source,1).get("world",{}).get("campaign_transition")=="slaver_camp",
         "manual shop card stores its destination")
    # Schema 3 saved the outstanding hand inside a shop. Resume a card from
    # that version and take the original choice after the next level starts.
    old_card = record(source, 1)
    module = old_card["session_mods"]["og-online-progression"]
    ledger = module["runtime"]
    assert ledger["open"]
    module["version"] = "3"
    ledger["v"] = 2
    for key in ("reward_generation", "round_completed", "match_completed"):
        ledger.pop(key)
    choices = ["health", "damage", "speed", "stealth_passive"]
    for row in ledger["players"] + ledger["teams"]:
        row.pop("luck")
    ledger["players"][0]["free_rewards"] = choices
    old_card["rules"]["mode_settings"][CAMPAIGN]["progression_free_buffs"] = 1
    card(source, 1).write_text(json.dumps(old_card))
    resumed=cold_resume(source,"campaign_shop_chapter_resume",28832,1,FIRST)
    source.stop()
    act(resumed,"ready")
    _ready(resumed,SECOND);_skip(resumed)
    wait(resumed,lambda:not view(resumed).get("shop_open",False),"ready advances the saved chapter shop")
    time.sleep(1)
    assert SECOND in resumed.bridge.status()["level"] and resumed.bridge.status()["online"]["challenge"]["loop"]==0
    migrated = view(resumed)
    assert [option["id"] for option in migrated["reward_cards"]] == choices
    assert migrated["stats"]["luck_rank"] == 0
    act(resumed, "reward", "damage")
    wait(resumed, lambda: view(resumed)["stats"]["damage_rank"] == 1, "legacy reward remains claimable in play")
    assert record(resumed,0).get("world",{}).get("campaign_transition") is None
    # Resolve the actual final chapter, preserve the chosen loop in an autosave,
    # then start a second fresh process with no campaign-script locals to reuse.
    _code(resumed,'LoadLevelID("sky_ark");')
    _ready(resumed,"27_Sky_Ark.xml");_skip(resumed)
    _code(resumed,'SendGlobalMessage("levelwin");')
    wait(resumed,lambda:view(resumed).get("shop_open",False) and view(resumed).get("final",False),"real final shop")
    act(resumed,"buy","luck")
    wait(resumed,lambda:view(resumed)["stats"]["luck_rank"] == 1,"buy Luck with free choices enabled")
    act(resumed,"loop","yes")
    _code(resumed,'SessionAutosave();SessionSave(2);')
    wait(resumed,lambda:record(resumed,0).get("session_mods",{}).get("og-online-progression",{}).get("runtime",{}).get("loop",False) and
         record(resumed,0).get("world",{}).get("campaign_transition") == "", "final autosave includes pending loop")
    wait(resumed,lambda:record(resumed,2).get("session_mods",{}).get("og-online-progression",{}).get("runtime",{}).get("loop",False),"final manual card includes loop")
    final=cold_resume(resumed,"campaign_shop_final_resume",28833,0,"27_Sky_Ark.xml",free_buffs=False)
    resumed.stop()
    assert view(final)["final"] and view(final)["loop"]
    assert "luck_rank" not in view(final)["stats"] and all(c["id"] != "luck" for c in view(final)["cards"])
    _code(final,'SessionSave(3);')
    wait(final,lambda:record(final,3).get("session_mods",{}).get("og-online-progression",{}).get("runtime",{}).get("loop"),
         "persist the purchased rank with free choices disabled")
    act(final,"ready")
    _ready(final,FIRST);_skip(final)
    wait(final,lambda:final.bridge.status()["online"]["challenge"]["loop"]==1,"saved final shop loops once")
    time.sleep(1.5)
    assert FIRST in final.bridge.status()["level"] and not view(final).get("shop_open",False)
    assert final.bridge.status()["online"]["challenge"]["loop"]==1
    _code(final,'SessionLoad(3);')
    wait(final,lambda:final.bridge.status()["online"]["lobby_waiting"] and not final.bridge.status()["level_loaded"],"saved lobby")
    _code(final,'LobbySetModeSetting("progression_free_buffs",1);LobbyStart();',"menu")
    _ready(final,"27_Sky_Ark.xml")
    wait(final,lambda:view(final).get("stats",{}).get("luck_rank") == 1,"re-enabled Luck retains its purchased rank")
    for inst in (source,resumed,final): assert not script_errors(inst),script_errors(inst)
    return {"passed":True,"metrics":{"manual_chapter_resume":True,"autosaved_final_resume":True,
            "resolved_destination":"slaver_camp","campaign_loops":1},"notes":[]}
