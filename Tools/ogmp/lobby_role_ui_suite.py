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
                    point = current['ui_nav_window_id'], current['ui_nav_id']
                    if point in visited:
                        break
                    visited.add(point)
                    move(direction, device)
            if focused(label, row):
                return
            shot('could_not_focus_' + label.replace(' ', '_'))
            raise AssertionError(f'{device} could not focus {label!r} on row {row}, last focus {point}')

        def focus_actions():
            # Hold X while moving through the window switcher with RB. The
            # selected window gets focus when X is released.
            inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=True)
            wait(lambda: inst.bridge.call('ui_state')['windowing_target'], 'open pad window switcher')
            for _ in range(8):
                if inst.bridge.call('ui_state')['windowing_target'] == 'Session actions':
                    break
                pad(10)
            inst.bridge.call('virtual_pad', action='button', slot=0, button=2, down=False)
            wait(lambda: state()['ui_nav_window'] == 'Session actions', 'pad reaches Players footer')

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
            pad(0)
            wait(lambda: len(rows()) == 2, 'unassigned controller joins as local P2')
            print('lobby_role_ui', transfer, 'keyboard host and pad P2 joined', flush=True)
            first = next(p for p in rows() if p['local_index'] == 0)
            second = next(p for p in rows() if p['local_index'] == 1)
            pads = inst.bridge.call('virtual_pad')['pads']
            assert pads[0]['local_index'] == 1, pads
            record['initial'] = state()
            assert state()['ui_nav_window'] != 'Edit player name' and probe('"" + editing_name') == '-1', \
                'join A activated the previously focused Rename button'
            # The join press belonged to P2; a keyboard arrow reclaims the UI
            # for P1 before selecting the actual promotion button.
            key('Up')
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

            if transfer != 'host_only':
                if transfer == 'host_and_leader':
                    # From P1's role actions, Down enters P2's row at Team.
                    seek('##team', second['id'], device='pad', directions=('down',))
                    seek('##screen', second['id'], device='pad', directions=('right',))
                else:
                    # With team editing restricted to the host, the next
                    # available selector on P2's row is Screen.
                    seek('##screen', second['id'], device='pad', directions=('down', 'up'))
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
                seek('Make host', first['id'], device='pad')
                seek('##team', first['id'], device='pad', directions=('up', 'right', 'down', 'left'), steps=3)
                activate('pad')
                pad(12)
                activate('pad')
                wait(lambda: next(p for p in rows() if p['id'] == first['id'])['team'] == 2,
                     'new host changes another player team through dropdown')
                record['host_team_changed'] = state()

            focus_actions()
            seek('Close lobby', device='pad')
            key('Down')
            wait(lambda: state()['menu_owner'] == 0, 'old keyboard player reclaims interaction')
            old_owner = ownership().split('|')
            assert int(old_owner[0]) == first['id'] and int(old_owner[1]) == 0, old_owner
            assert state()['online']['hosting'] and not state()['online']['client'], 'local promotion changed this machine into a guest'
            record['old_keyboard'] = {'state': state(), 'ownership': old_owner,
                                      'summary': probe('LobbyRoleSummary()')}
            shot('old_keyboard_after_transfer')
            if transfer == 'host_and_leader':
                assert old_owner[3:] == ['false', 'false', 'false'], old_owner
                key('Escape')
                time.sleep(0.25)
                assert state()['online']['active'], 'unprivileged old keyboard escaped the lobby'
                assert probe('"" + LobbyCanLeave()') == 'false'
                assert probe('notice') != '', 'denied keyboard Back gave no authority explanation'
                shot('old_keyboard_back_denied')
                activate('keyboard')
                time.sleep(0.25)
                assert state()['online']['active'], 'unprivileged old player closed the lobby'
                pad(12)
                wait(lambda: state()['menu_owner'] == 1, 'new host/leader pad reclaims interaction')
                seek('Close lobby', device='pad')
                shot('new_pad_close')
                activate('pad')
            else:
                assert old_owner[-1] == 'true', old_owner
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
