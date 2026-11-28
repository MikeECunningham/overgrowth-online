"""Controller scrolling and live text drafts, with process-local SDL input.

Run ``python3 -I Tools/ogmp.py run gui_navigation``. No desktop pointer,
keyboard, or attached physical controller is manipulated by this scenario.
"""
import os
import shutil
import tempfile
import time
import traceback
import zlib
from pathlib import Path

from .instance import ROOT
from .newcampaign_local_suite import _ready
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


def _native_focus(ctx):
    g = GUI(ctx, 'gui_native_focus')
    try:
        g.inst.bridge.call('menu', script='mainmenu.as')
        g.code('SetConfigValueInt("difficulty_preset_value", 0); imGUI.receiveMessage(IMMessage("Multiplayer"));')
        g.wait(lambda: g.ui()['nav_window'] == 'Multiplayer', 'first-run Multiplayer opens the role picker')
        host = g.ui()
        host_rect = host['nav_rect']
        window = next(w for w in host['windows'] if w['name'] == 'Multiplayer')
        host_id = zlib.crc32(b'Host\nCreate a lobby', window['id'])
        connect_id = zlib.crc32(b'Connect\nJoin a host', window['id'])
        assert host['nav_id'] == host_id
        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        def pad_right(down):
            g.inst.bridge.call('virtual_pad', action='button', slot=0, button=14, down=down)
        pad_right(True)
        g.wait(lambda: g.ui()['nav_id'] == connect_id, 'pad moves right to Connect')
        g.inst.bridge.call('key', key='Left', down=True)
        g.wait(lambda: g.ui()['nav_id'] == host_id, 'keyboard takes focus from held pad')
        time.sleep(0.6)  # Longer than the D-pad repeat delay.
        assert g.ui()['nav_id'] == host_id
        pad_right(False)
        pad_right(True)
        g.wait(lambda: g.ui()['nav_id'] == connect_id, 'new pad press takes focus from held key')
        time.sleep(0.6)
        assert g.ui()['nav_id'] == connect_id
        g.pointer(host_rect)
        g.wait(lambda: g.ui()['hovered_id'] == host_id, 'mouse hovers Host while both navigation inputs remain held')
        status = g.status()
        assert status['imgui_wants_mouse'] and status['desktop_cursor'] and not status['imgui_draws_cursor']
        g.click(host_rect)
        g.wait(lambda: g.status()['online']['lobby_waiting'], 'one mouse click opens Host')
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'host_id': host_id, 'connect_id': connect_id}
    finally:
        g.close()


def _native_draft(ctx):
    g = GUI(ctx, 'gui_native_draft')
    try:
        g.inst.bridge.call('menu', script='multiplayer_menu.as')
        time.sleep(0.25)
        g.key('Space')
        g.wait(lambda: g.status()['online']['lobby_waiting'], 'rendered Host')
        g.key('Space')  # The seat's initially focused profile selector.
        g.key('Down')
        g.key('Space')
        g.wait(lambda: g.probe('new_profile_seat') != '-1', 'inline New Profile editor')
        field = g.ui()['nav_rect']
        g.click(field)
        g.inst.bridge.call('text', text='HoverDraft')
        g.wait(lambda: g.probe('new_profile_name') == 'HoverDraft', 'live text input')
        draft = g.probe('new_profile_name')
        active = g.ui()['active_id']
        # Create sits directly below the inline editor. Moving there must
        # preserve the current text and keep focus until the click.
        create = [field[0], field[3] + 6, field[0] + 48, field[3] + 24]
        g.pointer(create)
        assert g.probe('new_profile_name') == draft
        assert g.ui()['active_id'] == active, 'hover stole text input focus'
        image = g.shot('native_live_draft_hover')
        g.click(create)
        g.wait(lambda: g.probe('new_profile_seat') == '-1', 'clicking Create saves the live draft')
        assert g.status()['online']['players'][0]['name'] == draft
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
        g.code('LobbySetSelection("com-wolfire-arena", "com-wolfire-arena", "Data/Levels/arenas/Magma_Arena.xml", false);')
        g.wait(lambda: any(w['name'] == 'Mode settings' for w in g.ui()['windows']), 'full hosting form')
        def move_to_form(button, form):
            for _ in range(16):
                if g.ui()['nav_window'].split('/')[0] == form:
                    return
                g.pad(button)
            raise AssertionError(f"D-pad could not reach {form}: {g.ui()}")

        move_to_form(13, 'Mode and map')
        move_to_form(14, 'Players')
        for _ in range(12):
            if g.ui()['nav_window'] in ('Hosting options', 'Mode settings'): break
            g.pad(14)
        if g.ui()['nav_window'] == 'Mode settings':
            move_to_form(12, 'Hosting options')
        assert g.ui()['nav_window'] == 'Hosting options'
        visited = []
        for _ in range(16):
            visited.append(g.ui())
            if visited[-1]['nav_id'] == zlib.crc32(b'Public game', next(w['id'] for w in visited[-1]['windows'] if w['name'] == 'Hosting options')):
                break
            g.pad(12)
        window = next(w for w in visited[-1]['windows'] if w['name'] == 'Hosting options')
        assert window['max_scroll_y'] > 0 and window['scroll_y'] > 0, 'Down did not scroll to offscreen controls'
        assert visited[-1]['nav_id'] == zlib.crc32(b'Public game', window['id']), 'last hosting option is unreachable'
        image = g.shot('native_controller_scrolled')
        for _ in range(16):
            if g.ui()['nav_id'] == zlib.crc32(b'Add slot', window['id']): break
            g.pad(11)
        top = next(w for w in g.ui()['windows'] if w['name'] == 'Hosting options')
        assert top['scroll_y'] < window['scroll_y'], 'Up did not scroll back toward the first option'
        assert g.ui()['nav_id'] == zlib.crc32(b'Add slot', top['id']), 'first hosting option is unreachable'
        assert g.probe('GetLocalKeyboardPlayerIndex()') == '-1'
        assert g.probe('LobbyAddBot() && LobbyStart()') == 'true'
        _ready(g.inst, 'Magma_Arena.xml')
        g.pad(4)
        g.wait(lambda: g.status()['open_menu_players'] == [0], 'controller host opens its pause menu')
        assert not g.status()['grab_mouse'], 'controller-only pause captured the unassigned mouse'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'bottom_scroll': window['scroll_y'], 'navigation': visited, 'screenshot': image}
    finally:
        g.close()


def _lobby_reload(ctx):
    # Edit only this instance's copy of the menu while it is hosting a lobby.
    with tempfile.TemporaryDirectory() as directory:
        mod = Path(directory) / 'session-rules'
        shutil.copytree(Path(ROOT) / 'test/mods/session-rules', mod)
        relative = Path('Data/Scripts/lobby_reload_check.as')
        (mod / relative).parent.mkdir(parents=True, exist_ok=True)
        source = (Path(ROOT) / 'Data/Scripts/multiplayer_lobby.as').read_text()
        (mod / relative).write_text(source + '\nint ReloadGeneration() {return 1;}\n'
                                    'class ReloadState { JSONValue data; }\nReloadState@ saved = ReloadState();\n')
        g = GUI(ctx, 'gui_lobby_reload', mods=[str(mod)])
        try:
            assert g.probe('LobbyBeginHost()') == 'true'
            g.inst.bridge.call('menu', script=relative.name)
            g.wait(lambda: any(w['name'] == 'Players' for w in g.ui()['windows']), 'lobby roster')
            script = Path(g.inst.writedir) / 'Data/Mods/session-rules' / relative
            for generation in (1, 2):
                g.code(f'saved.data["record"] = {generation * 17};')
                script.write_text(script.read_text().replace(f'ReloadGeneration() {{return {generation};',
                                                            f'ReloadGeneration() {{return {generation + 1};'))
                # The engine compares modification times to whole seconds.
                stamp = int(time.time()) + generation
                os.utime(script, (stamp, stamp))
                # F5 is an engine shortcut. Release Dear ImGui's keyboard
                # navigation capture as a mouse user returning to the game would.
                x = 8 + generation
                g.pointer([x, 28, x, 28])
                g.key('F5')
                g.wait(lambda: g.probe('ReloadGeneration()') == str(generation + 1), 'changed lobby script reloaded')
                assert g.probe('saved.data["record"].asInt()') == str(generation * 17)
                assert g.probe('lobby_players.length()') == '1'
            assert 'Unknown app object type:' not in g.inst.log()
        finally:
            g.close()


def _check_edge_drag(g):
    width, height = map(int, g.probe('GetScreenWidth()+"|"+GetScreenHeight()').split('|'))
    left = (width - float(g.probe('screenMetrics.screenSize.x'))) / 2
    # Capture starts in unoccupied menu space. A held drag may leave the SDL
    # window and re-enter it on either side, even with a controller present.
    path = [(6, 24), (-8, 28), (12, 32), (width - 4, 34), (width + 8, 38),
            (width - 12, height - 8), (width - 12, height + 8), (width - 12, height // 3)]
    for x, y in path:
        g.pointer([x, y, x, y], 1)
        p = 'screenMetrics.GUIToScreen(imGUI.guistate.mousePosition)'
        seen_x, seen_y = map(float, g.probe(p+'.x+"|"+'+p+'.y').split('|'))
        for sent, seen, limit in ((x, seen_x + left, width), (y, seen_y, height)):
            edge = max(0, min(sent, limit - 1))
            assert min(sent, edge) - 1 <= seen <= max(sent, edge) + 1, (x, y, seen_x + left, seen_y)
        assert not g.status()['grab_mouse'], 'menu captured the pointer during a drag'
    g.pointer([x, y, x, y])
    status = g.status()
    assert status['cursor_visible'] and status['desktop_cursor'] and not status['imgui_draws_cursor']


def _legacy_focus(ctx):
    g = GUI(ctx, 'gui_main_menu_focus')
    try:
        g.inst.bridge.call('menu', script='mainmenu.as')
        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        g.wait(lambda: g.probe('controller_items.length() > 0') == 'true', 'main menu buttons')
        _check_edge_drag(g)
        g.code('for(uint i=0;i<controller_items.length();++i){IMElement@ e=controller_items[i];'
               'if(e.controllerItem.getMessage().name == "Settings")'
               'SetConfigValueString("test_gui_probe",i+"|"+e.getScreenPosition().x+"|"+e.getScreenPosition().y+"|"+'
               'e.getSizeX()*screenMetrics.GUItoScreenXScale+"|"+e.getSizeY()*screenMetrics.GUItoScreenYScale);}SaveConfig();')
        index, x, y, w, h = map(float, g.stored_probe().split('|'))
        settings = [x, y, x + w, y + h]
        hovered = f'controller_items[{int(index)}].isMouseOver()'
        g.pointer(settings)
        assert g.probe(hovered) == 'true', 'parked pointer did not reach Settings'
        g.inst.bridge.call('virtual_pad', action='axis', slot=0, axis=1, value=5000)
        time.sleep(0.12)
        assert g.probe('GetMenuInputSource(0) == MenuMouse') == 'true', 'stick drift took focus'

        # Keep Down held throughout the handoffs. A stationary click should work
        # on the first try, even when the pad's selection is somewhere else.
        g.inst.bridge.call('virtual_pad', action='button', slot=0, button=12, down=True)
        time.sleep(0.12)
        assert g.probe('controller_active && GetMenuInputSource(0) == MenuController') == 'true'
        assert g.probe(hovered) == 'false', 'parked pointer retained its hover'
        g.key('Down')
        assert g.probe('!GetInputDown(0,"menu_down") && GetMenuInputSource(0) == MenuKeyboard') == 'true', 'held pad leaked into keyboard navigation'
        g.pad(11)
        assert g.probe('!GetInputDown(0,"menu_down") && GetMenuInputSource(0) == MenuController') == 'true', 'old Down resumed after a fresh Up'
        g.click(settings)
        g.wait(lambda: g.probe('controller_items.length() > 0 && controller_items[0].controllerItem.getMessage().name == "switch_category"') == 'true',
               'one click opens Settings')
        assert g.probe('current_screen') == 'Screen'
        assert g.probe('!controller_active && GetMenuInputSource(0) == MenuMouse') == 'true'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'opened': 'Settings'}
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
        g.inst.bridge.call('virtual_pad', action='attach', slot=0)
        _check_edge_drag(g)
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
        for key in ('1', 'Keypad 2', '3', 'Keypad 4'):
            g.key(key)
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

        g.pad(0)
        g.wait(lambda: g.status()['local_players'] == 2, 'actual controller joins')
        g.key('Escape')
        g.wait(lambda: g.status()['open_menu_players'] == [0], 'keyboard opens its split-screen pause menu')
        _check_edge_drag(g)
        g.code('this_ui.SendCallback("close");')
        g.wait(lambda: not g.status()['open_menu_players'], 'close split-screen keyboard menu')
        g.inst.bridge.call('pointer', enabled=False)
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


def _controller_host_join(ctx):
    gui = GUI(ctx, 'controller_host_join')
    try:
        gui.inst.bridge.call('menu', script='multiplayer_menu.as')
        gui.inst.bridge.call('virtual_pad', action='attach', slot=0)
        gui.wait(lambda: gui.ui()['nav_window'] == 'Multiplayer', 'role menu')
        gui.pad(0)
        gui.wait(lambda: gui.status()['online']['lobby_waiting'], 'controller opens host lobby')
        guest = ctx.instance('controller_host_guest', keys={'mp_port': '26983'})
        try:
            guest.bridge.call('connect', ip='127.0.0.1:26982')
            gui.wait(lambda: len(gui.status()['online']['players']) == 2, 'network guest joins')
            selected = set()
            for _ in range(4):
                gui.pad(12)
                selected.add(gui.ui()['nav_id'])
            assert len(selected) > 1 and 0 not in selected, selected
        finally:
            guest.stop()
    finally:
        gui.close()


def gui_navigation(ctx):
    metrics = {}
    try:
        _controller_host_join(ctx)
        metrics['native_focus'] = _native_focus(ctx)
        metrics['native_draft'] = _native_draft(ctx)
        metrics['native_scroll'] = _native_scroll(ctx)
        _lobby_reload(ctx)
        metrics['main_menu_focus'] = _legacy_focus(ctx)
        metrics['legacy'] = _legacy(ctx)
        return {'passed': True, 'metrics': metrics, 'notes': []}
    except Exception:
        return {'passed': False, 'metrics': metrics, 'notes': [traceback.format_exc()]}
