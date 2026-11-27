"""Exercise the actual Settings cards/checkboxes and the second couch seat's four shortcuts."""
import json
import shutil
import time
from pathlib import Path

from .gui_navigation_suite import GUI
from .scenarios import script_errors


def profile_menus(ctx):
    fixture = Path(ctx.out_dir, 'session-rules')
    shutil.copytree(Path(__file__).resolve().parents[2] / 'test/mods/session-rules', fixture, dirs_exist_ok=True)
    manifest = fixture / 'mod.xml'
    choices = ''.join(f'<Choice>{label}</Choice>' for label in
                      ('One', 'Two', 'Three', 'Four', 'Five', 'Six', 'Seven', 'Eight'))
    manifest.write_text(manifest.read_text().replace('</Mod>',
        '<ModSettings><Setting key="countdown" label="Countdown preset" type="choice">'
        + choices + '</Setting></ModSettings></Mod>'))
    g = GUI(ctx, 'profile_menus', mods=[str(fixture)])

    def click(name, collection='controller_items'):
        lookup = ('IMElement@ e=gui_elements[i].parent; string label=gui_elements[i].name;'
                  if collection == 'gui_elements' else
                  f'IMElement@ e={collection}[i]; string label=e.getName();')
        g.code('SetConfigValueString("test_gui_probe","missing");'
               f'for(uint i=0; i<{collection}.length(); ++i)' + '{' + lookup +
               f'if(label=="{name}")' + '{vec2 p=e.getScreenPosition();'
               'SetConfigValueString("test_gui_probe",p.x+"|"+p.y+"|"+e.getSizeX()*screenMetrics.GUItoScreenXScale+"|"+'
               'e.getSizeY()*screenMetrics.GUItoScreenYScale);break;}}SaveConfig();')
        box = g.stored_probe()
        assert box != 'missing', name
        x, y, w, h = map(float, box.split('|'))
        g.click((x,y,x+w,y+h))

    def axis(which):
        for depth in (32767, -32768):
            g.inst.bridge.call('virtual_pad', action='axis', slot=0, axis=which, value=depth)
            time.sleep(.14)

    try:
        g.inst.bridge.call('menu', script='main_menu_settings.as')
        time.sleep(.5)
        click('Category_Profiles')
        click('profile_action_New profile')
        for char in ('P','I','P','Keypad 1'):
            g.key(char)
        click('profile_action_Save')
        name = g.probe('settings_profile')
        assert name.lower() == 'pip1', name
        click('cosmetic_outfit.rabbit_guard')
        profile = lambda: json.loads(g.probe('ProfileView(settings_profile)'))
        assert profile()['selected']['outfit.rabbit'] == 'guard'
        click('profile_checkbox_primary')
        assert json.loads(g.probe('ProfileSettingsView()'))['primary'] == name
        click('profile_checkbox_primary')
        assert not json.loads(g.probe('ProfileSettingsView()'))['primary']
        click('Category_Mod settings')
        click('profile_action_Friendly Progression')
        click('profile_checkbox_meta_progression')
        click('Category_Profiles')
        assert profile()['meta'] and profile()['selected'].get('outfit.rabbit','turner') == 'turner'
        g.shot('profile_locked_cosmetics')
        click('Category_Mod settings')
        click('profile_checkbox_meta_progression')
        click('profile_action_Session rules test')
        click('profile_action_More choices')
        click('Countdown preset', 'gui_elements')
        click('dividerEight', 'sub_controller_items')
        mods = json.loads(g.probe('ProfileSettingsView()'))['mods']
        rules = next(mod for mod in mods if mod['id']=='session-rules')
        assert rules['settings'][0]['value']==7, rules
        click('Category_Profiles')
        assert not profile()['meta'] and profile()['selected']['outfit.rabbit'] == 'guard'
        mod = 'og-online-progression'
        g.code(f'ProfileSetModSetting("{mod}","meta_progression",1); ProfileSetModSetting("{mod}","upgrade_progression",1);')
        assert not profile()['can_roll'] and not profile()['can_roll_upgrade']
        # Cold-start a funded, isolated profile, keeping its real base and mod selections.
        g.inst.stop()
        archive = Path(g.inst.writedir, 'Data/player_profiles.json')
        saved = json.loads(archive.read_text())
        saved['profiles'][name]['mods'][mod]['collection']['tokens'] = 4
        archive.write_text(json.dumps(saved))
        g.inst.start(reuse_profile=True)
        g.inst.bridge.call('menu', script='main_menu_settings.as')
        time.sleep(.5)
        click('Category_Profiles')
        for kind in ('cosmetic', 'upgrade'):
            balance = profile()['tokens']
            click(f'profile_action_Roll {kind} (1)')
            view = profile()
            assert view['tokens'] == balance - 1 and view['win']['kind'] == kind
            g.code('SetConfigValueString("test_gui_probe","missing");'
                   'for(uint i=0;i<settings_content.getContainerCount();++i){'
                   'IMElement@ e = settings_content.getContainerAt(i).getContents();'
                   'if(e !is null && e.getName()=="profile_latest_unlock") SetConfigValueString("test_gui_probe","shown");'
                   '} SaveConfig();')
            assert g.stored_probe() == "shown"
        g.shot('profile_settings_last_unlock')
        g.inst.bridge.call('pointer', enabled=False)
        g.code('LobbyBeginHost();this_ui.SendCallback("multiplayer_lobby.as");')
        g.wait(lambda: g.status()['online']['lobby_waiting'], 'host lobby')
        g.wait(lambda:any(w['name'].startswith('Players/') for w in g.ui()['windows']), 'lobby roster drawn')
        seat = next(r['id'] for r in g.status()['online']['players'] if r['host'])
        def row():
            return json.loads(g.probe(f'LobbyProfileView({seat})'))
        def button_rect(kind):
            field = f'LobbyProfile({seat}).{kind}_roll_rect'
            box = g.probe('""+'+'+"|"+'.join(field+'.'+axis for axis in 'xyza'))
            x,y,x2,y2 = map(float,box.split('|'))
            return (x,y,x2,y2) if x2 > x and y2 > y else None
        def visible_button(kind):
            child = next(w for w in g.ui()['windows'] if w['name'].startswith('Players/'))
            left, top, right, bottom = child['rect']
            box = button_rect(kind)
            if box[3] > bottom - 1 and child['max_scroll_y'] > 0:
                x = right - 6
                start = (x,top+6,x,top+6)
                end = (x,bottom-4,x,bottom-4)
                g.pointer(start)
                g.pointer(start,1)
                g.pointer(end,1)
                g.pointer(end)
                box = button_rect(kind)
            content_right = right - (14 if child['max_scroll_y'] else 0)
            assert left <= box[0] < box[2] <= content_right and top <= box[1] < box[3] <= bottom, (kind,box,child)
            return box
        for kind in ('cosmetic', 'upgrade'):
            g.wait(lambda:button_rect(kind), f'guest has a {kind} roll button')
        assert row()['guest'] and not row()['can_roll'] and not row()['can_roll_upgrade']
        assert g.probe(f'LobbyProfileEdit({seat},"select","","{name}")') == 'true'
        for kind in ('cosmetic','upgrade'):
            balance = row()['tokens']
            g.wait(lambda:g.probe(f'LobbyProfile({seat}).reveal_until <= ImGui_GetTime()') == 'true',
                   'previous lobby award card clears')
            box = visible_button(kind)
            g.pointer(box)
            g.click(box)
            try:
                g.wait(lambda:row()['tokens'] == balance - 1, f'real lobby {kind} roll')
            except Exception:
                g.shot('failed_lobby_roll')
                evidence = {'button':kind,'rect':box,'ui':g.ui(),'hint':g.probe('lobby_visible_hint'),'row':row()}
                Path(ctx.out_dir,'roll_failure.json').write_text(json.dumps(evidence, indent=2))
                raise
            assert row()['win']['kind'] == kind
            assert g.probe(f'LobbyProfile({seat}).reveal_until > ImGui_GetTime()') == 'true'
        assert not row()['can_roll_upgrade'] and not row()['can_roll']
        assert g.probe(f'LobbyProfileEdit({seat},"select","","")') == 'true'
        g.code(f'ProfileSetModSetting("{mod}","meta_progression",0); ProfileSetModSetting("{mod}","upgrade_progression",0);')
        g.inst.bridge.call('pointer', enabled=False)
        g.inst.bridge.call('virtual_pad',action='attach',slot=0)
        time.sleep(.2)
        g.pad(0)
        g.wait(lambda: len(g.status()['online']['players'])==2,'second couch seat joins')
        other = next(r['id'] for r in g.status()['online']['players'] if r['local_index']==1)
        # The slot owner can select a host-local profile.
        assert g.probe(f'LobbyProfileEdit({other},"select","","{name}")') == 'true'
        axis(5)
        g.wait(lambda: g.probe(f'LobbyProfile({other}).category')=='1', 'R2 opens mouse outfits')
        other_view = lambda: json.loads(g.probe(f'LobbyProfileView({other})'))
        g.pad(10)
        g.wait(lambda: other_view()['selected'].get('outfit.rat')=='female_rat', 'R1 selects the next mouse outfit')
        g.pad(9)
        g.wait(lambda: other_view()['selected'].get('outfit.rat')=='rat', 'L1 selects the previous mouse outfit')
        axis(4)
        g.wait(lambda: g.probe(f'LobbyProfile({other}).category')=='0', 'L2 returns to rabbit outfits')
        assert other_view()['selected']['outfit.rabbit']=='guard'
        assert not script_errors(g.inst), script_errors(g.inst)
        return {'passed':True, 'metrics':{'settings_profile':name,'rabbit_outfit':'guard','couch_shortcuts':4,'settings_rolls':2,'lobby_rolls':2},'notes':[]}
    finally:
        g.close()
