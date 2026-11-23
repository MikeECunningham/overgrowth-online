"""Four difficulty cards and independently committed party-scaling controls."""

import time
import traceback

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
