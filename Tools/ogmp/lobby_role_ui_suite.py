"""Local role transfers through rendered controls and actual SDL inputs.

Keyboard P1 promotes pad P2 to host, local leader, or both. The new role's
controller must operate the permitted controls; session identity and Close
must follow their respective rules. No test call changes menu ownership or
performs a role transfer directly.
"""
import os
import struct
import time
import traceback
import zlib
from pathlib import Path

from .scenarios import script_errors


def lobby_role_ui(ctx):
    metrics = {}

    for transfer in ('host_only', 'leader_only', 'host_and_leader'):
        print('lobby_role_ui', transfer, 'starting', flush=True)
        inst = ctx.instance('role_ui_' + transfer, width=1280, height=720,
                            keys={'mp_port': '25844', 'playername': 'Keyboard P1',
                                  'master_volume': '0'})
        record = metrics[transfer] = {'navigation': []}

        def state():
            return inst.bridge.status()

        def rows():
            return state()['online']['players']

        def wait(predicate, label, seconds=20):
            return inst.bridge.wait(predicate, seconds, step=0.1, what=label)

        def code(text):
            return inst.bridge.call('exec', target='menu', code=text)

        def probe(expression):
            code('SetConfigValueString("test_role_ui_probe", "" + (' + expression + ')); SaveConfig();')
            config = dict(line.split(': ', 1) for line in (Path(inst.writedir) / 'Data/config.txt').read_text().splitlines()
                          if ': ' in line)
            return config['test_role_ui_probe']

        def shot(tag):
            path = os.path.abspath(os.path.join(ctx.out_dir, transfer + '_' + tag + '.png'))
            inst.bridge.call('screenshot', path=path)
            wait(lambda: os.path.isfile(path), 'role UI screenshot')
            record[tag + '_screenshot'] = path

        def key(name):
            inst.bridge.call('key', key=name, down=True)
            time.sleep(0.09)
            inst.bridge.call('key', key=name, down=False)
            time.sleep(0.09)

        def pad(button):
            inst.bridge.call('virtual_pad', action='button', slot=0, button=button, down=True)
            time.sleep(0.09)
            inst.bridge.call('virtual_pad', action='button', slot=0, button=button, down=False)
            time.sleep(0.09)

        def move(direction, device):
            if device == 'keyboard':
                key({'down': 'Down', 'up': 'Up', 'left': 'Left', 'right': 'Right'}[direction])
            else:
                pad({'up': 11, 'down': 12, 'left': 13, 'right': 14}[direction])
            assert len(rows()) == 2, 'ordinary menu navigation changed the two-player party'

        def activate(device):
            key('Space') if device == 'keyboard' else pad(0)
            if state()['online']['active']:
                assert len(rows()) == 2, 'menu activation unexpectedly admitted another player'

        def focused(label, row=None, current=None):
            if current is None:
                current = state()
            seed = int(current['ui_nav_window_id'])
            if row is not None:
                seed = zlib.crc32(struct.pack('<i', row), seed)
            expected = zlib.crc32(label.encode(), seed)
            return int(current['ui_nav_id']) == expected

        def seek(label, row=None, device='keyboard', directions=('down', 'up', 'down'), steps=14):
            # Read-only ImGui IDs identify the focused rendered widget. Ordinary
            # dpad/arrow navigation does all movement; no forced item focus.
            for direction in directions:
                visited = set()
                for _ in range(steps):
                    current = state()
                    record['navigation'].append({k: current.get(k) for k in
                        ('ui_nav_id', 'ui_nav_window_id', 'ui_nav_window', 'menu_owner', 'current_menu_player')})
                    if focused(label, row, current):
                        return
                    if device == 'pad' and row is not None and not current['ui_nav_window'].startswith('Players'):
                        focus_window('Players')
                        break
                    point = current['ui_nav_window_id'], current['ui_nav_id']
                    if point in visited:
                        break
                    visited.add(point)
                    move(direction, device)
            if focused(label, row):
                return
            shot('could_not_focus_' + label.replace(' ', '_'))
            raise AssertionError(f'{device} could not focus {label!r} on row {row}, last focus {point}')

        def focus_window(name):
            # Hold X while moving through the window switcher with RB. The
            # selected window gets focus when X is released.
            inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=True)
            wait(lambda: inst.bridge.call('ui_state')['windowing_target'], 'open pad window switcher')
            for _ in range(8):
                if inst.bridge.call('ui_state')['windowing_target'] == name:
                    break
                pad(10)
            inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=False)
            wait(lambda: state()['ui_nav_window'].split('/')[0] == name, 'pad reaches ' + name)

        def ownership():
            return probe('"" + LobbyCurrentPlayerID() + "|" + LobbyGetCurrentInputID() + "|" + '
                         'controller_player + "|" + LobbyIsHost() + "|" + LobbyCanManageLocal() + "|" + LobbyCanLeave()')

        try:
            inst.bridge.call('menu', script='multiplayer_menu.as')
            time.sleep(0.3)
            key('Space')
            wait(lambda: state()['online']['hosting'] and state()['online']['lobby_waiting'], 'keyboard chooses Host')
            inst.bridge.call('virtual_pad', action='attach', slot=0)
            time.sleep(0.15)
            assert len(rows()) == 1, 'plugging a controller in created a seat'
            pad(0)
            wait(lambda: len(rows()) == 2, 'unassigned controller joins as local P2')
            print('lobby_role_ui', transfer, 'keyboard host and pad P2 joined', flush=True)
            first = next(p for p in rows() if p['local_index'] == 0)
            second = next(p for p in rows() if p['local_index'] == 1)
            pads = inst.bridge.call('virtual_pad')['pads']
            assert pads[0]['local_index'] == 1, pads
            record['initial'] = state()
            assert probe('ImGui_IsAnyPopupOpen() || new_profile_seat >= 0 || editing_name >= 0') == 'false', \
                'join A activated the previously focused profile selector'
            assert state()['menu_owner'] == 0, 'joining pad stole lobby navigation'
            if transfer in ('host_only', 'host_and_leader'):
                seek('Make host', second['id'])
                activate('keyboard')
                wait(lambda: next(p for p in rows() if p['id'] == second['id'])['host'], 'rendered Make host transfers role')
            if transfer in ('leader_only', 'host_and_leader'):
                seek('Make local leader', second['id'])
                activate('keyboard')
                wait(lambda: next(p for p in rows() if p['id'] == second['id'])['leader'], 'rendered Make local leader transfers role')
            roles = {p['id']: p for p in rows()}
            assert roles[second['id']]['host'] == (transfer != 'leader_only'), roles
            assert roles[second['id']]['leader'] == (transfer != 'host_only'), roles
            assert roles[first['id']]['host'] == (transfer == 'leader_only'), roles
            assert roles[first['id']]['leader'] == (transfer == 'host_only'), roles
            print('lobby_role_ui', transfer, 'rendered promotion buttons passed', flush=True)

            pad(12)
            wait(lambda: state()['menu_owner'] == 1, 'new role holder pad owns current UI input')
            actual = inst.bridge.call('virtual_pad')['pads'][0]['input_id']
            claimed = ownership().split('|')
            assert list(map(int, claimed[:3])) == [second['id'], actual, actual], claimed
            record['new_owner'] = {'state': state(), 'ownership': claimed}

            focus_window('Players')
            if transfer != 'host_only':
                # The connected human's profile, team and screen are aligned.
                # A right press skips the disabled Team for a non-host leader.
                seek('##profile', second['id'], device='pad', directions=('up','left','down'))
                seek('##screen', second['id'], device='pad', directions=('right',))
                activate('pad')
                pad(11)  # Screen2 -> Screen1, through the rendered dropdown.
                activate('pad')
                wait(lambda: next(p for p in rows() if p['id'] == second['id'])['screen'] == 1,
                     'new local leader changes screen through dropdown')
                record['leader_screen_swap'] = rows()
            if transfer != 'leader_only':
                # Host-controls-teams is on. Changing the other human's team
                # therefore proves P2 gained host authority, independently of
                # whether P2 is also the local leader.
                if transfer == 'host_and_leader':
                    move('left', 'pad')
                    move('left', 'pad')
                seek('Make host', first['id'], device='pad', directions=('up', 'down'))
                if transfer == 'host_and_leader':
                    seek('##profile', first['id'], device='pad', directions=('up',))
                    seek('##team', first['id'], device='pad', directions=('right',))
                else:
                    seek('##team', first['id'], device='pad', directions=('up', 'right', 'down', 'left'), steps=3)
                activate('pad')
                pad(12)
                activate('pad')
                wait(lambda: next(p for p in rows() if p['id'] == first['id'])['team'] == 2,
                     'new host changes another player team through dropdown')
                record['host_team_changed'] = state()

            focus_window('Session actions')
            seek('Close lobby', device='pad')
            key('Down')
            if transfer == 'host_and_leader':
                assert state()['menu_owner'] == 1, 'unprivileged keyboard reclaimed the lobby'
                key('Escape')
                assert state()['online']['active'] and len(rows()) == 2, 'unprivileged Escape closed the lobby'
                key('Space')
                assert state()['online']['active'] and state()['menu_owner'] == 1
                assert probe('"" + LobbyCanLeave()') == 'true'
                seek('Close lobby', device='pad')
                activate('pad')
            else:
                wait(lambda: state()['menu_owner'] == 0, 'remaining role holder reclaims interaction')
                assert ownership().split('|')[-1] == 'true'
                seek('Close lobby')
                activate('keyboard')
            closed_at = time.monotonic()
            wait(lambda: not state()['online']['active'], 'rendered Close lobby actually stops session', 10)
            record['close_seconds'] = time.monotonic() - closed_at
            record['closed'] = state()
            assert not script_errors(inst), script_errors(inst)
            print('lobby_role_ui', transfer, 'PASS', flush=True)
        except Exception:
            record['error'] = traceback.format_exc()
            try:
                record['failed_state'] = state()
                shot('failure')
            except Exception:
                pass
            return {'passed': False, 'metrics': metrics, 'notes': [record['error']]}
        finally:
            if inst.bridge:
                try:
                    inst.bridge.call('virtual_pad', action='clear')
                except Exception:
                    pass
            inst.stop()
            (Path(ctx.out_dir) / (inst.name + '.log')).write_text(inst.log())

    # Dear ImGui captures keyboard navigation. An authorized Escape must still
    # reach the shared lobby's Back callback, rather than getting swallowed.
    inst = ctx.instance('role_ui_keyboard_back', width=640, height=360,
                        keys={'mp_port': '25844', 'playername': 'Keyboard Back', 'master_volume': '0'})
    record = metrics['authorized_keyboard_back'] = {}
    try:
        inst.bridge.call('menu', script='multiplayer_menu.as')
        time.sleep(0.3)
        key('Space')
        wait(lambda: state()['online']['hosting'], 'keyboard Back smoke hosts a lobby')
        ui = inst.bridge.call('ui_state')
        tabs = next(w['rect'] for w in ui['windows'] if w['name'] == '##lobby_sections')
        # Players is the middle tab before a mode is selected.
        x, y = int((tabs[0] + tabs[2]) / 2), int((tabs[1] + tabs[3]) / 2)
        for buttons in (0, 1, 0):
            inst.bridge.call('pointer', x=x, y=y, buttons=buttons)
            time.sleep(0.08)
        wait(lambda: state()['ui_nav_window'] == 'Players', 'short viewport opens the Players tab')
        inst.bridge.call('pointer', enabled=False)
        for _ in range(14):
            if focused('Close lobby'):
                break
            key('Down')
        assert focused('Close lobby'), 'short viewport could not scroll from the roster to Close'
        record['before'] = state()
        started = time.monotonic()
        key('Escape')
        wait(lambda: not state()['online']['active'], 'authorized captured keyboard Escape closes the lobby', 10)
        record['seconds'] = time.monotonic() - started
        record['after'] = state()
        assert not script_errors(inst), script_errors(inst)
        print('lobby_role_ui authorized keyboard Back PASS', flush=True)
    except Exception:
        record['error'] = traceback.format_exc()
        return {'passed': False, 'metrics': metrics, 'notes': [record['error']]}
    finally:
        inst.stop()
        (Path(ctx.out_dir) / (inst.name + '.log')).write_text(inst.log())
    return {'passed': True, 'metrics': metrics, 'notes': []}


def lobby_device_claims(ctx):
    """Two idle pads on each machine, keyboard Host, second pad Connect."""
    from .mode_settings_suite import _code, _probe

    host, guest = (ctx.instance("device_" + role, keys={"mp_port": str(port), "master_volume": "0"})
                   for role, port in (("host", 29743), ("guest", 29744)))

    def press(inst, button, slot=1):
        for down in (True, False):
            inst.bridge.call("virtual_pad", action="button", slot=slot, button=button, down=down)
            time.sleep(.10)

    def key(inst, name):
        for down in (True, False):
            inst.bridge.call("key", key=name, down=down)
            time.sleep(.10)

    def players(inst):
        return inst.bridge.status()["online"]["players"]

    def assignments(inst):
        return [p["local_index"] for p in inst.bridge.call("virtual_pad")["pads"]]

    for inst in (host, guest):
        for slot in (0, 1):
            inst.bridge.call("virtual_pad", action="attach", slot=slot)
    host.bridge.call("menu", script="multiplayer_menu.as")
    host.bridge.wait(lambda: host.bridge.call("ui_state")["nav_window"] == "Multiplayer", 20, what="host button")
    key(host, "Space")
    host.bridge.wait(lambda: len(players(host)) == 1 and assignments(host) == [-1,-1], 20, what="only keyboard host")
    assert _probe(host, "GetLocalKeyboardPlayerIndex()") == "0"

    guest.bridge.call("menu", script="multiplayer_menu.as")
    guest.bridge.wait(lambda: guest.bridge.call("ui_state")["nav_window"] == "Multiplayer", 20, what="role picker")
    press(guest, 14)
    press(guest, 0)
    guest.bridge.wait(lambda: any(w["name"] == "Connect to a host" for w in guest.bridge.call("ui_state")["windows"]),
                      20, what="pad opens connect form")
    _code(guest, 'address="127.0.0.1:29743";', "menu")
    # Clicking the address form's submit button must preserve the selected pad.
    ui = guest.bridge.call("ui_state")
    window = next(w for w in ui["windows"] if w["name"] == "Connect to a host")
    left, top, right, bottom = window["rect"]
    x = int((left + right) * .5)
    target = zlib.crc32(b"Connect", window["id"])
    for y in range(int(top + 8), int(bottom), 12):
        guest.bridge.call("pointer", x=x, y=y, buttons=0)
        time.sleep(.06)
        if guest.bridge.call("ui_state")["hovered_id"] == target: break
    else:
        raise AssertionError("Connect button unreachable")
    for buttons in (1, 0):
        guest.bridge.call("pointer", x=x, y=y, buttons=buttons)
        time.sleep(.10)
    guest.bridge.wait(lambda: len(players(guest)) == 2 and assignments(guest) == [-1,0],
                      30, what="activating pad owns client leader only")
    assert _probe(guest, "GetLocalKeyboardPlayerIndex()") == "-1"
    guest.bridge.call("virtual_pad", action="axis", slot=0, axis=0, value=23000)
    time.sleep(.25)
    assert assignments(guest) == [-1,0] and len(players(host)) == 2
    press(guest, 0, slot=0)
    host.bridge.wait(lambda: len(players(host)) == 3 and assignments(guest) == [1,0], 15, what="intentional pad join")
    assert guest.bridge.status()["menu_owner"] == 0
    assert not script_errors(host) and not script_errors(guest)
    return {"passed": True, "metrics": {"host_pads": assignments(host), "client_pads": assignments(guest)}, "notes": []}
