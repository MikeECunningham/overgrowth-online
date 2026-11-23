"""Controller scrolling and live text drafts, with process-local SDL input.

Run ``python3 -I Tools/ogmp.py run gui_navigation``. No desktop pointer,
keyboard, or attached physical controller is manipulated by this scenario.
"""
import os
import time
import traceback
import zlib
from pathlib import Path

from .scenarios import LEVEL, script_errors, wait_loaded


class GUI:
    def __init__(self, ctx, name, **options):
        self.ctx = ctx
        self.inst = ctx.instance(name, width=1280, height=720,
                                 keys={'master_volume': '0', 'mp_port': '26982',
                                       'custom_resolution': 'true'}, **options)
        self.player = None

    def status(self):
        return self.inst.bridge.status()

    def ui(self):
        return self.inst.bridge.call('ui_state')

    def wait(self, predicate, label):
        return self.inst.bridge.wait(predicate, 20, what=label)

    def code(self, code, target=None):
        target = target or ('menu' if self.player is None else 'player_menu')
        options = {'local_player': self.player} if target == 'player_menu' else {}
        return self.inst.bridge.call('exec', target=target, code=code, **options)

    def stored_probe(self):
        path = Path(self.inst.writedir) / 'Data/config.txt'
        return dict(line.split(': ', 1) for line in path.read_text().splitlines()
                    if ': ' in line)['test_gui_probe']

    def probe(self, expression):
        self.code('SetConfigValueString("test_gui_probe", "" + (' + expression + ')); SaveConfig();')
        return self.stored_probe()

    def key(self, name):
        for down in (True, False):
            self.inst.bridge.call('key', key=name, down=down)
            time.sleep(0.09)

    def pad(self, button):
        for down in (True, False):
            self.inst.bridge.call('virtual_pad', action='button', slot=0, button=button, down=down)
            time.sleep(0.09)

    def pointer(self, rect, buttons=0):
        self.inst.bridge.call('pointer', x=int((rect[0] + rect[2]) * 0.5),
                              y=int((rect[1] + rect[3]) * 0.5), buttons=buttons)
        time.sleep(0.12)

    def click(self, rect):
        self.pointer(rect, 1)
        self.pointer(rect)

    def shot(self, tag):
        path = os.path.abspath(os.path.join(self.ctx.out_dir, tag + '.png'))
        self.inst.bridge.call('screenshot', path=path)
        self.wait(lambda: os.path.isfile(path), 'GUI screenshot')
        return path

    def close(self):
        if self.inst.bridge:
            try:
                self.inst.bridge.call('pointer', enabled=False)
                self.inst.bridge.call('virtual_pad', action='clear')
            except Exception:
                pass
        self.inst.stop()
        (Path(self.ctx.out_dir) / (self.inst.name + '.log')).write_text(self.inst.log())


def _native_draft(ctx):
    g = GUI(ctx, 'gui_native_draft')
    try:
        g.inst.bridge.call('menu', script='multiplayer_menu.as')
        time.sleep(0.25)
        g.key('Space')
        g.wait(lambda: g.status()['online']['lobby_waiting'], 'rendered Host')
        g.key('Space')  # Initial selected Rename button, using the keyboard.
        g.wait(lambda: g.probe('editing_name') != '-1', 'name editor')
        ui = g.ui()
        field = ui['nav_rect']
        window = next(w['rect'] for w in ui['windows'] if w['name'] == 'Edit player name')
        # Last row is the form's ordinary Save button, above its16px padding.
        save = [window[0] + 16, window[3] - 50, window[0] + 146, window[3] - 16]
        g.click(field)
        g.inst.bridge.call('text', text='HoverDraft')
        time.sleep(0.15)
        draft = g.probe('name_buffer')
        assert 'HoverDraft' in draft, draft
        active = g.ui()['active_id']
        g.pointer(save)
        assert g.probe('name_buffer') == draft, 'hover reset the unsaved name'
        assert g.ui()['active_id'] == active, 'hover stole text input focus'
        image = g.shot('native_live_draft_hover')
        g.click(save)
        g.wait(lambda: g.probe('editing_name') == '-1', 'rendered Save')
        assert g.status()['online']['players'][0]['name'] == draft, 'Save used the pre-edit value'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'draft': draft, 'screenshot': image}
    finally:
        g.close()


def _native_scroll(ctx):
    g = GUI(ctx, 'gui_native_scroll')
    try:
        g.inst.bridge.call('menu', script='multiplayer_menu.as')
        time.sleep(0.25)
        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        time.sleep(0.15)
        g.pad(0)
        g.wait(lambda: g.status()['online']['lobby_waiting'], 'controller chooses Host')
        for _ in range(8):
            if g.ui()['nav_window'] == 'Hosting options':
                break
            g.inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=True)
            time.sleep(0.09)
            g.pad(10)
            g.inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=False)
            time.sleep(0.18)
        visited = []
        for _ in range(16):
            visited.append(g.ui())
            g.pad(12)
        window = next(w for w in visited[-1]['windows'] if w['name'] == 'Hosting options')
        assert window['max_scroll_y'] > 0 and window['scroll_y'] > 0, 'Down did not scroll to offscreen controls'
        assert visited[-1]['nav_id'] == zlib.crc32(b'Close lobby', window['id']), 'Close remains inaccessible'
        image = g.shot('native_controller_scrolled')
        for _ in range(16):
            g.pad(11)
        top = next(w for w in g.ui()['windows'] if w['name'] == 'Hosting options')
        assert top['scroll_y'] < window['scroll_y'], 'Up did not scroll back toward the first option'
        assert g.ui()['nav_id'] == zlib.crc32(b'Everyone may pause', top['id']), 'first hosting option is unreachable'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'bottom_scroll': window['scroll_y'], 'navigation': visited, 'screenshot': image}
    finally:
        g.close()


def _legacy(ctx):
    g = GUI(ctx, 'gui_legacy_scroll_draft', level=LEVEL)
    try:
        wait_loaded(g.inst)
        g.wait(lambda: not g.status()['waiting_for_input'], 'loaded world ready')
        time.sleep(0.2)
        g.key('Escape')
        g.wait(lambda: g.status()['open_menu_players'] == [0], 'keyboard pause menu')
        g.player = 0
        g.code('imGUI.receiveMessage(IMMessage("Settings"));')
        g.wait(lambda: g.probe('personal_settings') == 'true', 'settings page')
        g.code('SwitchSettingsScreen("Screen");')
        time.sleep(0.15)

        def numeric():
            g.code('for(uint i=0;i<gui_elements.length();++i){if(gui_elements[i].name=="textbox_screenwidth") {'
                   'TextboxNumeric@ f=cast<TextboxNumeric>(gui_elements[i]);'
                   'SetConfigValueString("test_gui_probe",f.query+"|"+f.element_open+"|"+f.unsaved_values);}}SaveConfig();')
            return g.stored_probe()

        def element_rect(name, slider=False):
            item = 'cast<Slider>(gui_elements[i]).slider_image' if slider else 'gui_elements[i].container'
            g.code('for(uint i=0;i<gui_elements.length();++i){if(gui_elements[i].name=="' + name + '") {'
                   'IMElement@ e=' + item + ';SetConfigValueString("test_gui_probe",'
                   'e.getScreenPosition().x+"|"+e.getScreenPosition().y+"|"+'
                   'e.getSizeX()*screenMetrics.GUItoScreenXScale+"|"+e.getSizeY()*screenMetrics.GUItoScreenYScale);}}SaveConfig();')
            x, y, width, height = map(float, g.stored_probe().split('|'))
            return [x, y, x + width, y + height]

        g.click(element_rect('textbox_screenwidth'))
        for digit in '1234':
            g.key(digit)
        assert numeric() == '1234|true|true', numeric()
        g.pointer(element_rect('Brightness', slider=True))
        assert g.probe('active_slider') != '-1', 'fixture did not hover the slider'
        assert numeric() == '1234|true|true', 'hover ended/reset numeric entry'
        g.code('imGUI.receiveMessage(IMMessage("Settings_refresh"));')
        time.sleep(0.2)
        assert numeric() == '1234|true|true', 'settings rebuild lost a draft'
        g.code('IMMessage m("option_changed");m.addString("Window mode");m.addString("0");imGUI.receiveMessage(m);')
        time.sleep(0.2)
        assert numeric() == '1234|true|true', 'another option discarded numeric entry'
        draft_image = g.shot('legacy_numeric_draft_hover')
        g.key('Escape')
        assert numeric() == '1280|false|false', 'explicit cancel did not restore configured resolution'
        assert g.probe('personal_settings') == 'true', 'cancel also closed settings'
        g.code('this_ui.SendCallback("close");')
        g.wait(lambda: not g.status()['open_menu_players'], 'close P1 menu')
        g.inst.bridge.call('pointer', enabled=False)

        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        time.sleep(0.15)
        g.pad(0)
        g.wait(lambda: g.status()['local_players'] == 2, 'actual controller joins')
        g.pad(4)  # Configured quit/menu button, SDL Back.
        g.wait(lambda: g.status()['open_menu_players'] == [1], 'joined controller opens its menu')
        g.player = 1
        g.pad(12)
        g.pad(0)
        g.wait(lambda: g.probe('personal_settings') == 'true', 'controller Settings')
        g.pad(12)
        g.pad(14)  # Leave the category column for the keybind list.
        down = []
        for _ in range(24):
            down.append(g.probe('show_keybinds_offset+"|"+GetCurrentControllerItemName()'))
            g.pad(12)
        assert '5|rebind_gamepad_1_grab' in down, 'Down skipped the hidden final bindings'
        key_image = g.shot('legacy_controller_last_bindings')
        up = []
        for _ in range(26):
            up.append(g.probe('show_keybinds_offset+"|"+GetCurrentControllerItemName()'))
            g.pad(11)
        assert '0|rebind_gamepad_1_up' in up, 'Up did not restore the first binding'

        # P2 has neither host nor leader authority: these are read-only rows.
        g.code('for(int n=0;n<4;++n) LobbyAddBot();', target='level')
        g.wait(lambda: len(g.status()['online']['players']) == 6, 'large roster')
        g.code('BuildPlayerTeamsMenu();')
        team_down = []
        for _ in range(10):
            team_down.append(g.probe('team_scroll_offset+"|"+GetCurrentControllerItemName()'))
            g.pad(12)
        assert any(item.startswith('2|team_row_5') for item in team_down), 'read-only Teams rows cannot scroll'
        team_image = g.shot('legacy_controller_teams_bottom')
        for _ in range(10):
            g.pad(11)
        assert g.probe('team_scroll_offset') == '0', 'Teams cannot scroll back up'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'numeric_draft': '1234', 'keybind_down': down, 'keybind_up': up,
                'teams_down': team_down,
                'screenshots': [draft_image, key_image, team_image]}
    finally:
        g.close()


def gui_navigation(ctx):
    metrics = {}
    try:
        metrics['native_draft'] = _native_draft(ctx)
        metrics['native_scroll'] = _native_scroll(ctx)
        metrics['legacy'] = _legacy(ctx)
        return {'passed': True, 'metrics': metrics, 'notes': []}
    except Exception:
        return {'passed': False, 'metrics': metrics, 'notes': [traceback.format_exc()]}
