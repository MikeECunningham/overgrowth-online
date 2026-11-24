"""Four difficulty cards and independently committed party-scaling controls."""

import math
import os
import time
import traceback
from pathlib import Path

from .gui_navigation_suite import GUI
from .scenarios import script_errors


def difficulty_ui(ctx):
    g = GUI(ctx, 'campaign_difficulty_ui')
    metrics = {}
    try:
        g.code('SetConfigValueInt("difficulty_preset_value",0);SetConfigValueBool("campaign_mp_scaling",true);')
        g.inst.bridge.call('menu', script='difficulty_menu.as')
        g.wait(lambda: g.probe('play_menu.length()') == '4', 'four actual difficulty cards')
        assert g.probe('play_menu[3].name') == 'Nightmare'
        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        time.sleep(0.2)
        g.pad(14)

        def focus_message(name, argument=None):
            test = f'm.name=="{name}"'
            if argument is not None:
                test += f'&&m.getString(0)=="{argument}"'
            g.code('for(uint i=0;i<controller_items.length();++i){'
                   'IMMessage@ m=controller_items[i].controllerItem.getMessage();'
                   f'if(m !is null&&{test})' + '{SetCurrentControllerItem(i);break;}}')

        # This draft may be edited before selecting a first-time preset.
        focus_message('option_changed', 'Scale campaign encounters for party size')
        g.pad(0)
        assert g.probe('campaign_scaling') == 'false'
        assert g.probe('GetConfigValueBool("campaign_mp_scaling")') == 'true', 'draft applied before Continue'
        assert g.probe('current_difficulty') == '-1'

        g.code('SetCurrentControllerItem("menu_item0");')
        navigation = []
        for _ in range(3):
            g.pad(14)
            navigation.append(g.probe('GetCurrentControllerItemName()'))
        assert navigation == ['menu_item1', 'menu_item2', 'menu_item3'], navigation
        g.pad(0)
        g.wait(lambda: g.probe('current_difficulty') == '3', 'controller chooses Nightmare')
        assert g.probe('campaign_scaling') == 'false', 'preset choice changed the separate scaling draft'

        # Follow the actual vertical controller route to the added checkbox.
        down = []
        for _ in range(8):
            g.pad(12)
            selected = g.probe('current_item !is null&&current_item.controllerItem.getMessage() !is null ? '
                               'current_item.controllerItem.getMessage().name+"|"+'
                               '(current_item.controllerItem.getMessage().name=="option_changed" ? '
                               'current_item.controllerItem.getMessage().getString(0) : "") : ""')
            down.append(selected)
            if selected == 'option_changed|Scale campaign encounters for party size':
                break
        assert 'option_changed|Scale campaign encounters for party size' in down, down
        metrics['screenshot'] = g.shot('nightmare_and_party_scaling')
        focus_message('Continue')
        g.pad(0)
        g.wait(lambda: g.probe('GetConfigValueInt("difficulty_preset_value")') == '4', 'apply fourth preset')
        assert g.probe('GetConfigValueString("difficulty_preset")') == 'Nightmare'
        assert g.probe('GetConfigValueBool("campaign_mp_nightmare")') == 'true'
        assert g.probe('GetConfigValueBool("campaign_mp_scaling")') == 'false'
        assert g.probe('GetConfigValueFloat("game_difficulty")') == '1'
        assert g.probe('GetConfigValueFloat("global_time_scale_mult")') == '1'

        # Reopening uses the current native preset; Back cancels its new draft.
        g.inst.bridge.call('menu', script='difficulty_menu.as')
        g.wait(lambda: g.probe('current_difficulty') == '3', 'reopen authoritative fourth preset')
        focus_message('option_changed', 'Scale campaign encounters for party size')
        g.pad(0)
        assert g.probe('campaign_scaling') == 'true'
        focus_message('Back')
        g.pad(0)
        assert g.probe('GetConfigValueBool("campaign_mp_scaling")') == 'false', 'Back committed a draft'
        assert not script_errors(g.inst), script_errors(g.inst)
        metrics.update({'right_navigation': navigation, 'down_navigation': down,
                        'nightmare_expert_base': True, 'scaling_commit_and_cancel': True})
        return {'passed': True, 'metrics': metrics, 'notes': []}
    except Exception:
        return {'passed': False, 'metrics': metrics, 'notes': [traceback.format_exc()]}
    finally:
        g.close()


def difficulty_checkbox_transition(ctx):
    """Unclipped checkbox artwork survives gameplay and a fresh lobby.

    The original failure kept the labels and working input but clipped out the
    box and checkmark images. Compare actual checkbox pixels in both states,
    rather than accepting a successful state toggle as proof of rendering.
    """
    from PIL import Image
    from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready

    inst = ctx.instance("difficulty_checkbox_transition", width=3840, height=2024,
                        keys={"mp_port": "28054", "master_volume": "0", "multisample": "1",
                              "texture_reduce": "3", "detail_objects": "0", "particle_field": "0",
                              "no_reflection_capture": "true", "depth_of_field": "0",
                              "limit_fps_in_game": "true", "max_frame_rate": "30",
                              "custom_resolution": "true"})
    metrics = {}
    names = ("Automatic Ledge Grab", "Tutorials", "Scale campaign encounters for party size")

    def code(text, target="menu", **options):
        return inst.bridge.call("exec", target=target, code=text, **options)

    def wait(predicate, label):
        return inst.bridge.wait(predicate, 30, step=0.1, what=label)

    def probe(expression):
        code('SetConfigValueString("test_gui_probe",""+(' + expression + '));SaveConfig();')
        values = dict(line.split(": ", 1) for line in
                      (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        return values["test_gui_probe"]

    def boxes():
        code('string rows;for(uint i=0;i<gui_elements.length();++i){GUIElement@ e=gui_elements[i];'
             'vec2 p=e.parent.getScreenPosition();'
             # The legacy renderer centers its 16:9 canvas on wider windows.
             'p.x+=max(0.0f,(GetScreenWidth()-GetScreenHeight()*16.0f/9.0f)*0.5f);'
             'rows+=e.name+"|"+e.option_state+"|"+'
             'p.x+"|"+p.y+"|"+e.parent.getSizeX()*screenMetrics.GUItoScreenXScale+"|"+'
             'e.parent.getSizeY()*screenMetrics.GUItoScreenYScale+";";}'
             'SetConfigValueString("test_gui_probe",rows);SaveConfig();')
        values = dict(line.split(": ", 1) for line in
                      (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        result = {}
        for row in values["test_gui_probe"].split(";"):
            if not row:
                continue
            name, state, x, y, width, height = row.split("|")
            x, y, width, height = map(float, (x, y, width, height))
            result[name] = {"checked": state == "true",
                            "rect": [math.floor(x), math.floor(y), math.ceil(x + width), math.ceil(y + height)]}
        assert set(result) == set(names), result
        return result

    def set_boxes(checked):
        for name, row in boxes().items():
            if row["checked"] != checked:
                code('IMMessage m("option_changed");m.addString("' + name + '");'
                     'm.addString("");imGUI.receiveMessage(m);')
        wait(lambda: all(row["checked"] == checked for row in boxes().values()), "checkbox GUI state")
        assert probe('ledge_grab&&tutorials&&campaign_scaling' if checked else
                     '!ledge_grab&&!tutorials&&!campaign_scaling') == "true"

    def capture(tag):
        time.sleep(0.2)
        rows = boxes()
        path = os.path.abspath(os.path.join(ctx.out_dir, tag + ".png"))
        if os.path.exists(path):
            os.unlink(path)
        inst.bridge.call("screenshot", path=path)
        images = []

        def image_ready():
            try:
                with Image.open(path) as source:
                    images.append(source.convert("L"))
                return True
            except (OSError, ValueError):
                return False

        wait(image_ready, "complete checkbox screenshot")
        image = images[0]
        metrics[tag] = {"screenshot": path, "boxes": rows}
        return {name: image.crop(row["rect"]).resize((64, 64)) for name, row in rows.items()}

    def pair(tag):
        set_boxes(False)
        off = capture(tag + "_unchecked")
        set_boxes(True)
        on = capture(tag + "_checked")
        return off, on

    try:
        code('SetConfigValueString("difficulty_preset","Nightmare");SetConfigValueInt("difficulty_preset_value",4);'
             'SetConfigValueBool("auto_ledge_grab",false);SetConfigValueBool("tutorials",false);'
             'SetConfigValueBool("campaign_mp_scaling",true);')
        inst.bridge.call("menu", script="difficulty_menu.as")
        wait(lambda: probe("gui_elements.length()") == "3", "fresh difficulty checkboxes")
        fresh_off, fresh_on = pair("fresh")

        inst.bridge.call("menu", script="mainmenu.as")
        code('LobbyBeginHost();LobbyAddBot();LobbyAddBot();'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();')
        _ready(inst, FIRST)
        inst.bridge.call("input", controller=0, hold={"quit": 1}, steps=1)
        wait(lambda: 0 in inst.bridge.status()["open_menu_players"], "campaign pause menu")
        code('imGUI.receiveMessage(IMMessage("Main Menu"));', "player_menu", local_player=0)
        wait(lambda: not inst.bridge.status()["online"]["active"], "campaign exits to main menu")
        time.sleep(0.8)
        code('LobbyBeginHost();this_ui.SendCallback("multiplayer_lobby.as");')
        wait(lambda: any(w["name"] == "Players" for w in inst.bridge.call("ui_state")["windows"]), "new lobby")
        code('this_ui.SendCallback("difficulty_menu.as");')
        wait(lambda: probe("gui_elements.length()") == "3", "difficulty after gameplay and lobby transition")
        returned_off, returned_on = pair("returned")

        checks = {}
        for name in names:
            base_off, base_on = list(fresh_off[name].getdata()), list(fresh_on[name].getdata())
            off, on = list(returned_off[name].getdata()), list(returned_on[name].getdata())
            checkmark = [i for i, (a, b) in enumerate(zip(base_off, base_on)) if b - a > 30]
            outline = [i for i, (a, b) in enumerate(zip(base_off, base_on)) if a > 100 and abs(b - a) < 20]
            assert len(checkmark) > 40 and len(outline) > 40, (name, "fresh checkbox template missing", len(checkmark), len(outline))
            checkmark_visible = sum(on[i] - off[i] > 20 for i in checkmark) / len(checkmark)
            outline_visible = sum(off[i] >= base_off[i] * 0.65 for i in outline) / len(outline)
            checks[name] = {"checkmark_visible": round(checkmark_visible, 3), "outline_visible": round(outline_visible, 3)}
            assert checkmark_visible > 0.65, (name, "checkmark vanished after game/menu transition", checks[name])
            assert outline_visible > 0.75, (name, "unchecked outline vanished after game/menu transition", checks[name])
        metrics["visible_pixels"] = checks
        assert not script_errors(inst), script_errors(inst)
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        return {"passed": False, "metrics": metrics, "notes": [traceback.format_exc()]}
    finally:
        inst.stop()
        (Path(ctx.out_dir) / (inst.name + ".log")).write_text(inst.log())
