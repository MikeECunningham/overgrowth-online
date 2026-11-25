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

    def cold_resume(source, name, port, slot, level):
        fresh=instance(name,port)
        for folder in ("SessionSaves","Textures/SessionSaves"):
            shutil.copytree(Path(source.writedir,"Data",folder),Path(fresh.writedir,"Data",folder),dirs_exist_ok=True)
        _code(fresh,f'SetConfigValueBool("test_shop_resume",SessionLoad({slot}));',"menu")
        wait(fresh,lambda:fresh.bridge.status()["online"]["lobby_waiting"],"saved shop opens its lobby")
        wait(fresh,lambda:fresh.bridge.call("progression")["ledger"]["open"],"saved shop ledger arrives in the lobby")
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
          'LobbySetModeSetting("challenge_time",0);LobbyStart();',"menu")
    _ready(source,FIRST);_skip(source)
    _code(source,'SendGlobalMessage("levelwin");')
    wait(source,lambda:view(source).get("shop_open",False),"real chapter completion opens shop")
    wait(source,lambda:record(source,0).get("world",{}).get("campaign_transition")=="slaver_camp",
         "shop autosave stores the already resolved next chapter")
    _code(source,'SessionSave(1);')
    wait(source,lambda:record(source,1).get("world",{}).get("campaign_transition")=="slaver_camp",
         "manual shop card stores its destination")
    assert record(source,1)["session_mods"]["og-online-progression"]["runtime"]["open"]
    resumed=cold_resume(source,"campaign_shop_chapter_resume",28832,1,FIRST)
    source.stop()
    act(resumed,"ready")
    _ready(resumed,SECOND);_skip(resumed)
    wait(resumed,lambda:not view(resumed).get("shop_open",False),"ready advances the saved chapter shop")
    time.sleep(1)
    assert SECOND in resumed.bridge.status()["level"] and resumed.bridge.status()["online"]["challenge"]["loop"]==0
    assert record(resumed,0).get("world",{}).get("campaign_transition") is None
    # Resolve the actual final chapter, preserve the chosen loop in an autosave,
    # then start a second fresh process with no campaign-script locals to reuse.
    _code(resumed,'LoadLevelID("sky_ark");')
    _ready(resumed,"27_Sky_Ark.xml");_skip(resumed)
    _code(resumed,'SendGlobalMessage("levelwin");')
    wait(resumed,lambda:view(resumed).get("shop_open",False) and view(resumed).get("final",False),"real final shop")
    act(resumed,"loop","yes")
    _code(resumed,'SessionAutosave();SessionSave(2);')
    wait(resumed,lambda:record(resumed,0).get("session_mods",{}).get("og-online-progression",{}).get("runtime",{}).get("loop",False) and
         record(resumed,0).get("world",{}).get("campaign_transition") == "", "final autosave includes pending loop")
    wait(resumed,lambda:record(resumed,2).get("session_mods",{}).get("og-online-progression",{}).get("runtime",{}).get("loop",False),"final manual card includes loop")
    final=cold_resume(resumed,"campaign_shop_final_resume",28833,0,"27_Sky_Ark.xml")
    resumed.stop()
    assert view(final)["final"] and view(final)["loop"]
    act(final,"ready")
    _ready(final,FIRST);_skip(final)
    wait(final,lambda:final.bridge.status()["online"]["challenge"]["loop"]==1,"saved final shop loops once")
    time.sleep(1.5)
    assert FIRST in final.bridge.status()["level"] and not view(final).get("shop_open",False)
    assert final.bridge.status()["online"]["challenge"]["loop"]==1
    for inst in (source,resumed,final): assert not script_errors(inst),script_errors(inst)
    return {"passed":True,"metrics":{"manual_chapter_resume":True,"autosaved_final_resume":True,
            "resolved_destination":"slaver_camp","campaign_loops":1},"notes":[]}
