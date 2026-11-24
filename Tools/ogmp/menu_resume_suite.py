"""Night Raid menus must release gameplay without discarding an explicit pause.

Uses real Escape/W events, a mouse click on Settings, authored cinematics, and
the pause menu's Retry. Actors are never made static by this fixture.
"""

import math
import time
import traceback
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, _probe, _ready, _skip
from .scenarios import script_errors


NIGHT = "og_story/04_Main_Slaver_Camp.xml"


def campaign_menu_resume(ctx):
    metrics = {}
    host = None
    offline = None

    def exercise(inst, label, network):
        def status():
            return inst.bridge.call("status", timeout=240)

        def wait(predicate, what, seconds=30):
            return inst.bridge.wait(predicate, seconds, step=0.08, what=what)

        def code(text, target="level"):
            return inst.bridge.call("exec", target=target, code=text)

        def key(name):
            for down in (True, False):
                inst.bridge.call("key", key=name, down=down)
                time.sleep(0.12)

        def hero():
            return next(c for c in inst.bridge.state() if c["controlled"] and not c["remote"])

        def player_probe(expression):
            code('SetConfigValueString("test_menu_resume_probe",""+(' + expression + '));', "player_menu")
            return _probe(inst, 'GetConfigValueString("test_menu_resume_probe")')

        def animation():
            actor = hero()
            return _probe(inst, f'ReadCharacterID({actor["id"]}).rigged_object().anim_client().GetCurrAnim()')

        def bindings():
            return _probe(inst, 'GetBindingValue("key","up")+"|"+GetBindingValue("key","down")+"|"+'
                          'GetBindingValue("key","left")+"|"+GetBindingValue("key","right")+"|"+'
                          'GetBindingValue("key","jump")+"|"+GetBindingValue("key","attack")')

        def close_settings(phase):
            key("Escape")
            wait(lambda: status()["open_menu_players"] == [0], phase + " opens the actual pause menu")
            # Bots do not turn a sole human's lobby into a shared-human game.
            assert status()["paused"], (phase, "sole human's menu did not pause", status())
            code('for(uint i=0;i<controller_items.size();++i){IMElement@ e=controller_items[i];'
                 'IMMessage@ m=e.controllerItem.getMessage();if(m !is null && m.name=="Settings") {'
                 'vec2 p=e.getScreenPosition();SetConfigValueString("test_menu_resume_box",""+'
                 '(p.x+e.getSizeX()*screenMetrics.GUItoScreenXScale*0.5f)+"|"+'
                 '(p.y+e.getSizeY()*screenMetrics.GUItoScreenYScale*0.5f));break;}}', "player_menu")
            x, y = map(float, _probe(inst, 'GetConfigValueString("test_menu_resume_box")').split("|"))
            for buttons in (0, 1, 0):
                inst.bridge.call("pointer", x=int(x), y=int(y), buttons=buttons)
                time.sleep(0.15)
            wait(lambda: player_probe("personal_settings") == "true", phase + " mouse click enters Settings")
            assert player_probe("active_rebind") == "-1", "Settings click leaked into a key rebinding"
            code('SwitchSettingsScreen("Game");', "player_menu")
            time.sleep(0.65)
            key("Escape")
            wait(lambda: player_probe("personal_settings") == "false", phase + " leaves settings without edits")
            fault = None
            if phase == "during_intro":
                fault = inst.bridge.call("player_pose_fault", char=hero()["id"], script_counter=1200)
            key("Escape")
            wait(lambda: not status()["open_menu_players"] and not status()["paused"], phase + " releases gameplay")
            inst.bridge.call("pointer", enabled=False)
            assert bindings() == initial_bindings, phase + " changed the gameplay bindings"
            if fault:
                time.sleep(0.3)
                assert status()["dialogue_camera_control"] and animation() == "", "menu recovery overwrote a cinematic pose"
                inst.bridge.call("player_pose_fault", char=fault["actor"], restore=fault["before"])
                metrics[label]["cinematic_pose_preserved"] = True

        def movement(phase):
            before = hero()
            assert before["knocked_out"] == 0 and not before["dialogue_control"] and not before["coop_cutscene_hold"], before
            assert not status()["paused"] and not status()["dialogue_camera_control"], status()
            initial_animation = animation()
            assert initial_animation, (phase, "actor remains in an uninitialized pose")
            inst.bridge.call("key", key="W", down=True)
            time.sleep(0.45)
            moving_animation = animation()
            inst.bridge.call("key", key="W", down=False)
            after = hero()
            distance = math.dist(before["pos"], after["pos"])
            assert distance > 0.2 and after["updated"] > before["updated"], (phase, before, after)
            assert moving_animation, (phase, "movement animation is missing")
            metrics[label][phase] = {"movement_m": distance, "updated_delta": after["updated"] - before["updated"],
                                      "idle_animation": initial_animation, "moving_animation": moving_animation}

        def pose_guard(phase, expected, **options):
            key("Escape")
            wait(lambda: status()["open_menu_players"] == [0], phase + " opens menu")
            fault = inst.bridge.call("player_pose_fault", char=hero()["id"], script_counter=1200, **options)
            try:
                assert fault["after"]["animation"] == "", fault
                key("Escape")
                wait(lambda: not status()["open_menu_players"], phase + " closes menu")
                if expected:
                    wait(lambda: animation() != "", phase + " initializes the missing local idle pose", 2)
                    assert not hero()["proxy"], "owned actor retained stale network proxy state"
                else:
                    time.sleep(0.4)
                    assert animation() == "", phase + " unexpectedly modified an excluded actor"
                    if options.get("owner", -1) >= 0:
                        assert hero()["proxy"], "resume repair took over another peer's simulation ownership"
                metrics[label][phase] = {"recovered": expected, "animation": animation()}
            finally:
                inst.bridge.call("player_pose_fault", char=fault["actor"], restore=fault["before"])
            wait(lambda: animation() != "", phase + " fixture restores ordinary animation")

        metrics[label] = {}
        initial_bindings = bindings()
        wait(lambda: status()["dialogue_camera_control"], label + " authored opening cinematic")
        close_settings("during_intro")
        _skip(inst)
        wait(lambda: not status()["dialogue_camera_control"] and not status()["coop_cutscene"], "opening dialogue releases the actors")
        movement("after_intro_menu")
        close_settings("during_play")
        movement("after_play_menu")
        pose_guard("empty_pose", True)
        pose_guard("authored_static_pose", False, static=True)
        pose_guard("held_pose", False, hold=True)
        if network:
            pose_guard("owned_stale_proxy", True, proxy=True)
            pose_guard("temporary_foreign_owner", False, proxy=True, owner=0)

        if network:
            key("Escape")
            wait(lambda: status()["open_menu_players"] == [0], "host reopens its menu")
            # Solo menus auto-pause and have no separate shared-world button.
            # An explicit session freeze must nevertheless survive their close.
            code('LobbySetPaused(true);')
            wait(lambda: status()["paused"], "host explicitly pauses the shared world")
            key("Escape")
            wait(lambda: not status()["open_menu_players"], "host closes its explicitly paused menu")
            time.sleep(0.2)
            assert status()["paused"], "closing a personal menu discarded an explicit shared pause"
            key("Escape")
            wait(lambda: bool(status()["open_menu_players"]), "paused world still permits reopening the menu")
            code('LobbySetPaused(false);')
            assert status()["paused"], "automatic menu pause vanished with explicit freeze"
            key("Escape")
            wait(lambda: not status()["open_menu_players"] and not status()["paused"], "resume menu closes")
            movement("after_explicit_resume")

        # Retry must use the same route as the player's menu, not a fabricated reset.
        # An existing level-context string survives until the actual reload;
        # has_display_text remains false, so this marker is never rendered.
        code('display_text="menu-resume-before-retry";')
        assert _probe(inst, "display_text") == "menu-resume-before-retry"
        inst.bridge.call("level_messages", prefix="reset")
        reset_seen = False
        key("Escape")
        wait(lambda: bool(status()["open_menu_players"]), "host opens Retry")
        if network:
            code('LobbySetPaused(true);')
            wait(lambda: status()["paused"], "explicitly paused world before Retry")
        code('imGUI.receiveMessage(IMMessage("Retry"));', "player_menu")

        def reloaded():
            nonlocal reset_seen
            state = status()
            if not state["level_loaded"] or NIGHT not in state["level"]:
                return False
            if state["waiting_for_input"]:
                inst.bridge.call("start_session")
                return False
            if not network:
                # True offline Retry restores the checkpoint in the existing
                # level script; an online host reloads the scene for its peers.
                reset_seen = reset_seen or "reset" in inst.bridge.call("level_messages", prefix="reset")["messages"]
                return reset_seen and not state["open_menu_players"]
            try:
                return _probe(inst, "display_text") != "menu-resume-before-retry"
            except AssertionError:
                # The queued reload may dispose the old AS context between
                # status and exec; wait for the replacement context to respond.
                return False

        wait(reloaded, "Retry restores the attempt using its mode's reset route", 240)
        assert not status()["paused"], "Retry retained the previous attempt's world pause"
        _skip(inst)
        wait(lambda: not status()["dialogue_camera_control"] and not status()["coop_cutscene"], "Retry's cinematic ends")
        movement("after_retry")
        assert bindings() == initial_bindings
        assert not script_errors(inst), script_errors(inst)
        metrics[label]["renderer"] = status()["renderer"]

    try:
        host = ctx.instance("campaign_menu_resume_host", width=960, height=540,
                            keys={**RENDER_KEYS, "mp_port": "28094", "player_invincible": "true", "tutorials": "false",
                                  "difficulty_scaling": "false", "no_auto_nav_mesh": "true"})
        host.bridge.call("exec", target="menu", code='LobbyBeginHost();for(int i=0;i<7;++i)LobbyAddBot();'
                         f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{NIGHT}",false);LobbyStart();')
        _ready(host, NIGHT)
        exercise(host, "host_with_bots", True)
        host.stop()

        offline = ctx.instance("campaign_menu_resume_solo", width=960, height=540,
                               keys={**RENDER_KEYS, "no_auto_nav_mesh": "true", "player_invincible": "true", "tutorials": "false",
                                     "difficulty_scaling": "false"})
        offline.bridge.call("load_level", path="set_campaign " + CAMPAIGN)
        offline.bridge.call("load_level", path="load_campaign_level slaver_camp_night")
        _ready(offline, NIGHT)
        assert not offline.bridge.status()["online"]["active"]
        exercise(offline, "offline_solo", False)
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        return {"passed": False, "metrics": metrics, "notes": [traceback.format_exc()]}
    finally:
        for inst in (host, offline):
            if inst:
                inst.stop()
                (Path(ctx.out_dir) / (inst.name + ".log")).write_text(inst.log())


def lobby_menu_pause(ctx):
    from .newcampaign_local_suite import FIRST
    host = ctx.instance("shortcut_host", keys={**RENDER_KEYS, "mp_port": "28671"})
    guest = ctx.instance("shortcut_guest", keys={**RENDER_KEYS, "mp_port": "28672"})
    def script(inst, code, where="level"):
        inst.bridge.call("exec", target=where, code=code)
    def key(inst, name):
        inst.bridge.call("key", key=name, down=True)
        time.sleep(.13)
        inst.bridge.call("key", key=name, down=False)
        time.sleep(.1)
    def expect(inst, menu, paused):
        def ready():
            s = inst.bridge.status()
            return bool(s["open_menu_players"]) == menu and s["paused"] == paused
        try:
            inst.bridge.wait(ready, 6, what=f"{inst.name} menu={menu} paused={paused}")
        except Exception as exc:
            s = inst.bridge.status()
            raise AssertionError({"instance":inst.name,"expected":[menu,paused],
                                  "actual":[s["open_menu_players"],s["paused"]],
                                  "inputs":s["local_inputs"],"cutscene":s["coop_cutscene"],
                                  "dialogue":s["dialogue_camera_control"]}) from exc
    script(host, 'LobbyBeginHost();LobbyAddBot();'
           f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();', "menu")
    _ready(host, FIRST)
    _skip(host)
    key(host, "Escape")
    expect(host, True, True)
    key(host, "Escape")
    expect(host, False, False)
    guest.bridge.call("connect", ip="127.0.0.1:28671")
    _ready(guest, FIRST)
    key(host, "Escape")
    expect(host, True, False)
    key(host, "Escape")
    expect(host, False, False)
    key(guest, "p")
    expect(guest, True, False)
    expect(host, False, False)
    key(guest, "Escape")
    expect(guest, False, False)
    script(guest,'LobbySetPaused(true);')
    expect(host,False,False)
    expect(guest,False,False)
    script(host, 'SetBindingValue("key","walk","p");')
    key(host, "p")
    expect(host, False, False)
    script(host, 'SetBindingValue("key","walk","lctrl");')
    key(host, "p")
    expect(host, True, True)
    expect(guest, False, True)
    key(host, "Escape")
    expect(host, False, False)
    expect(guest, False, False)
    script(host, 'LobbySetPaused(true);')
    key(host, "p")
    expect(host, True, True)
    key(host, "Escape")
    expect(host, False, True)
    script(host, 'LobbySetPaused(false);')
    expect(guest, False, False)
    # The lobby option grants guests the same menu-owned pause lifetime. A
    # second menu can join that pause, and closing either alone must retain it.
    script(host,'LobbyEndGame();')
    host.bridge.wait(lambda:all(i.bridge.status()["online"]["lobby_waiting"] and
                               not i.bridge.status()["level_loaded"] for i in (host,guest)),
                     30,what="pause policy lobby")
    script(host,'LobbySetAllowAllPause(true);LobbyStart();',"menu")
    for inst in (host,guest): _ready(inst,FIRST)
    _skip(host)
    key(guest,"p")
    expect(guest,True,True)
    expect(host,False,True)
    key(host,"p")
    expect(host,True,True)
    key(guest,"Escape")
    expect(guest,False,True)
    key(host,"Escape")
    expect(host,False,False)
    expect(guest,False,False)
    # Holding P opens once, while an explicit freeze survives a shortcut menu.
    guest.bridge.call("key",key="p",down=True)
    time.sleep(.6)
    guest.bridge.call("key",key="p",down=False)
    expect(guest,True,True)
    key(guest,"Escape")
    expect(guest,False,False)
    script(host,'LobbySetPaused(true);')
    expect(guest,False,True)
    key(guest,"p")
    expect(guest,True,True)
    key(guest,"Escape")
    expect(guest,False,True)
    script(host,'LobbySetPaused(false);')
    expect(guest,False,False)
    assert not script_errors(host) and not script_errors(guest)
    return {"passed": True, "metrics": {"solo_escape": True, "shared_shortcut": True,
            "rebind_respected": True, "explicit_pause_preserved": True,
            "guest_permissions":True,"multiple_menu_owners":True,"held_shortcut_opens_once":True}, "notes": []}
