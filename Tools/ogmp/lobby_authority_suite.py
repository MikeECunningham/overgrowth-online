"""Transfer local administration and the actual server, then play on its new host.

All peers use distinct listening ports. The preflight-failure case deliberately
asks a candidate to bind the still-running original host's port.
"""
import math
import time
from pathlib import Path

from .scenarios import LEVEL


def lobby_authority(ctx):
    host = ctx.instance('authority_host', keys={'mp_port': '27344', 'playername': 'Keyboard Host'})
    candidate = ctx.instance('authority_candidate', keys={'mp_port': '27345', 'playername': 'Candidate One', 'gamepad_0_look_sensitivity': '2.75', 'game_difficulty': '0.1'})
    third = ctx.instance('authority_third', keys={'mp_port': '27346', 'playername': 'Third'})
    instances = [host, candidate, third]
    metrics = {}

    def code(inst, text, target='menu', local=None):
        args = {'target': target, 'code': text}
        if local is not None:
            args['local_player'] = local
        return inst.bridge.call('exec', **args)

    def status(inst):
        return inst.bridge.status()

    def rows(inst):
        return status(inst)['online']['players']

    def wait(inst, condition, description, timeout=40):
        return inst.bridge.wait(condition, timeout, what=description)

    def config(inst):
        return dict(line.split(': ', 1) for line in (Path(inst.writedir) / 'Data/config.txt').read_text().splitlines()
                    if ': ' in line)

    def probe(inst, expression, actual=None, target='menu', local=None):
        prefix = '' if actual is None else f'SetControllerPlayer({actual}); '
        code(inst, prefix + 'SetConfigValueBool("authority_test_result", (' + expression + ')); SaveConfig();', target, local)
        return config(inst).get('authority_test_result') in ('true', '1')

    def find(inst, name):
        return next(p for p in rows(inst) if p['name'] == name)

    def wait_roster(n, peers=instances):
        for inst in peers:
            wait(inst, lambda i=inst: len(rows(i)) == n, f'{n} admitted seats on {inst.name}')

    def row_identity(inst):
        return {p['id']: (p['name'], p['team'], p['screen'], p['local_index'], p['leader'], p['host']) for p in rows(inst)}

    code(host, 'LobbyBeginHost(); LobbyAddLocalPlayer();')
    code(candidate, 'LobbyAddLocalPlayer();')
    # Leave a gap in the original transport IDs, which migration must not
    # confuse with preserved logical IDs when later clients join.
    third.bridge.call('connect', ip='127.0.0.1:27344')
    wait(host, lambda: len(rows(host)) == 3, 'temporary first peer admitted')
    third.bridge.call('disconnect')
    wait(third, lambda: not status(third)['online']['active'], 'temporary peer leaves')
    wait(host, lambda: len(rows(host)) == 2, 'temporary peer removed')
    for inst in [candidate, third]:
        inst.bridge.call('connect', ip='127.0.0.1:27344')
    wait_roster(5)
    initial = rows(host)
    hp = next(p for p in initial if p['peer_id'] == 255 and p['local_index'] == 1)
    cp = next(p for p in rows(candidate) if p['name'] == 'Candidate One')
    cp2 = next(p for p in initial if p['peer_id'] == cp['peer_id'] and p['local_index'] == 1)
    tp = next(p for p in initial if p['name'] == 'Third')
    assert probe(host, f'LobbySetPlayerName({hp["id"]}, "Pad Host")')
    assert probe(candidate, f'LobbySetPlayerName({cp2["id"]}, "Candidate Two")')
    assert probe(candidate, f'LobbySetPlayerScreen({cp2["id"]}, 1)')
    assert probe(host, f'LobbySetSelection("sandbox", "", "Data/Levels/{LEVEL}", false)')
    wait(host, lambda: find(host, 'Candidate Two')['screen'] == 1, 'client screen permutation reaches host')
    assert not probe(candidate, f'LobbySetPlayerTeam({cp2["id"]}, 2)', actual=1)
    assert probe(host, 'LobbySetHostControlsTeams(false)', actual=0)
    wait(candidate, lambda: not status(candidate)['online']['host_controls_teams'], 'team policy replicates')
    assert probe(candidate, f'LobbySetPlayerTeam({cp2["id"]}, 2)', actual=1)
    assert not probe(candidate, f'LobbySetPlayerTeam({tp["id"]}, 3)', actual=1)
    wait(host, lambda: find(host, 'Candidate Two')['team'] == 2, 'own-team request accepted')
    # Local roles transfer independently; neither grant moves any input device.
    assert probe(host, f'LobbyPromoteHost({hp["id"]})', actual=0)
    assert probe(host, f'LobbyPromoteLeader({hp["id"]})', actual=0)
    wait(candidate, lambda: find(candidate, 'Pad Host')['host'] and find(candidate, 'Pad Host')['leader'], 'roles replicate')
    code(host, 'SetConfigValueFloat("gamepad_1_look_sensitivity", 3.25); SaveConfig();')
    assert probe(host, 'LobbyRemovePlayer(255)', actual=1)
    wait_roster(4)
    assert find(host, 'Pad Host')['id'] == hp['id']
    assert find(host, 'Pad Host')['local_index'] == 0
    assert probe(host, 'LobbyIsHost() && LobbyCanManageLocal() && LobbyCurrentPlayerID() == ' + str(hp['id']), actual=1)
    assert config(host).get('gamepad_0_look_sensitivity') == '3.25'
    assert status(host)['local_inputs'] == 1
    metrics['after_primary_removal'] = rows(host)

    print('authority: local roles and primary removal PASS', flush=True)
    # A blocked new listening port fails without destroying the existing lobby.
    code(candidate, 'SetConfigValueInt("mp_port", 27344);')
    assert probe(host, f'LobbyPromoteHost({cp["id"]})', actual=1)
    wait(host, lambda: not status(host)['online']['migration_busy'] and
         bool(status(host)['online']['migration_status']), 'unavailable candidate port reported', 30)
    assert status(host)['online']['hosting'] and status(host)['online']['decision_host'] == hp['id']
    wait_roster(4)
    assert status(candidate)['online']['client'] and status(third)['online']['client']
    metrics['failed_preflight_status'] = status(host)['online']['migration_status']

    print('authority: failed preflight preserves lobby PASS', flush=True)
    code(candidate, 'SetConfigValueInt("mp_port", 27345);')
    code(host, 'SetConfigValueInt("oo_arena_bots", 3); SetConfigValueBool("oo_arena_random", false); '
         'SetConfigValueBool("oo_public_game", false); SetConfigValueFloat("game_difficulty", 0.9f); '
         'SetConfigValueFloat("global_time_scale_mult", 0.8f); SetConfigValueBool("tutorials", false); '
         'SetConfigValueBool("auto_ledge_grab", false); SaveConfig();')
    before = row_identity(host)
    assert probe(host, f'LobbyPromoteHost({cp["id"]})', actual=1)
    for inst in instances:
        wait(inst, lambda i=inst: len(rows(i)) == 4 and not status(i)['online']['migration_busy'] and
             status(i)['online']['decision_host'] == cp['id'], 'three-node server handoff', 70)
    assert status(candidate)['online']['hosting']
    assert status(host)['online']['client'] and status(third)['online']['client']
    for inst in instances:
        after = row_identity(inst)
        for player_id, expected in before.items():
            # Host role moves; logical identity, screen, team and leader survive.
            assert after[player_id][:-1] == expected[:-1], (inst.name, expected, after[player_id])
        assert sum(p['host'] for p in rows(inst)) == 1
    code(candidate, 'SaveConfig();')
    migrated_config = config(candidate)
    for key, expected in {'oo_arena_bots': 3, 'oo_arena_random': 0, 'oo_public_game': 0, 'game_difficulty': 0.9,
                          'global_time_scale_mult': 0.8, 'tutorials': 0, 'auto_ledge_grab': 0}.items():
        assert abs(float(migrated_config[key]) - expected) < 0.0001, (key, migrated_config[key])
    assert abs(float(migrated_config['gamepad_0_look_sensitivity']) - 2.75) < 0.0001, 'private controller settings migrated'
    metrics['migrated_match_settings'] = {key: migrated_config[key] for key in
        ['oo_arena_bots', 'oo_arena_random', 'oo_public_game', 'game_difficulty', 'global_time_scale_mult', 'tutorials', 'auto_ledge_grab']}
    assert probe(candidate, 'Online_GetOwnPlayerID() == 0 && Online_GetPlayerName(0) == "Candidate One"')
    assert probe(host, 'Online_GetOwnPlayerID() == ' + str(hp['id']))
    metrics['migrated_roster'] = rows(candidate)
    print('authority: three-node host handoff and settings PASS', flush=True)
    host.stop()
    live = [candidate, third]
    wait_roster(3, live)
    assert status(candidate)['online']['hosting']
    late = ctx.instance('authority_late_guest', keys={'mp_port': '27347', 'playername': 'Late Guest'})
    late.bridge.call('connect', ip='127.0.0.1:27345')
    wait_roster(4, live + [late])
    assert find(candidate, 'Third')['id'] == tp['id']
    late_row = find(candidate, 'Late Guest')
    assert late_row['peer_id'] == tp['id'] and late_row['id'] != tp['id'], late_row
    metrics['late_join_collision_avoided'] = {'preserved_player': tp['id'], 'new_player': late_row['id'], 'new_peer': late_row['peer_id']}
    late.stop()
    wait_roster(3, live)
    assert probe(candidate, 'LobbyCanStart() && LobbyStart()', actual=0)
    for inst in live:
        wait(inst, lambda i=inst: status(i)['level_loaded'] and not status(i)['waiting_for_input'],
             'match on new server after original host disconnects', 180)
    # A personal menu's actor may edit only their own team under the shared policy.
    third.bridge.call('input', controller=0, hold={'quit': 1.0}, steps=1)
    wait(third, lambda: 0 in status(third)['open_menu_players'], 'remote personal team menu')
    assert probe(third, f'LobbySetPlayerTeam({tp["id"]}, 3)', target='player_menu', local=0)
    assert not probe(third, f'LobbySetPlayerTeam({cp["id"]}, 3)', target='player_menu', local=0)
    wait(candidate, lambda: find(candidate, 'Third')['team'] == 3, 'in-match own team replicated')
    third.bridge.call('exec', target='player_menu', local_player=0, code='this_ui.SendCallback("close");')
    # Local leader can swap screens while input ownership remains unchanged.
    candidate.bridge.call('input', controller=1, hold={'quit': 1.0}, steps=1)
    wait(candidate, lambda: 1 in status(candidate)['open_menu_players'], 'P2 private team menu')
    assert not probe(candidate, f'LobbySetPlayerScreen({cp2["id"]}, 2)', target='player_menu', local=1)
    candidate.bridge.call('input', controller=0, hold={'quit': 1.0}, steps=1)
    wait(candidate, lambda: 0 in status(candidate)['open_menu_players'], 'leader private team menu')
    assert probe(candidate, f'LobbySetPlayerScreen({cp2["id"]}, 2)', target='player_menu', local=0)
    wait(candidate, lambda: find(candidate, 'Candidate Two')['screen'] == 2, 'live screen swap')
    wait(third, lambda: find(third, 'Candidate Two')['screen'] == 2, 'live screen plan replicated')
    actors = {p['common_id']: p for p in candidate.bridge.state()}
    p2row = find(candidate, 'Candidate Two')
    p2 = next(p for p in actors.values() if not p['proxy'] and p['controller_id'] == 1)
    assert p2['camera_id'] == 1, p2
    assert probe(candidate, 'LobbySetHostControlsTeams(true)', target='player_menu', local=0)
    wait(third, lambda: status(third)['online']['host_controls_teams'], 'in-match host-only team policy')
    assert not probe(candidate, f'LobbySetPlayerTeam({cp2["id"]}, 3)', target='player_menu', local=1)
    candidate.bridge.call('exec', target='player_menu', local_player=1, code='this_ui.SendCallback("close");')
    candidate.bridge.call('exec', target='player_menu', local_player=0, code='this_ui.SendCallback("end_game");')
    for inst in live:
        wait(inst, lambda i=inst: status(i)['online']['lobby_waiting'] and not status(i)['level_loaded'], 'return migrated lobby', 60)
    metrics['post_match_roster'] = rows(candidate)
    print('authority: migrated match, private teams and live screens PASS', flush=True)
    # Natural Arena assignments remain visible and protected from self edits,
    # while the host can explicitly regroup a fighter for the current match.
    assert probe(candidate, 'LobbySetHostControlsTeams(false) && LobbySetForceTeams(false) && '
                 'LobbySetSelection("com-wolfire-arena", "com-wolfire-arena", "Data/Levels/arenas/Magma_Arena.xml", false)')
    assert probe(candidate, 'LobbyStart()')
    for inst in live:
        wait(inst, lambda i=inst: status(i)['level_loaded'] and not status(i)['waiting_for_input'], 'natural Arena starts', 180)
    p2avatar = next(p for p in rows(candidate) if p['id'] == cp2['id'])['avatar_id']
    wait(candidate, lambda: any(p['id'] == p2avatar and p['teams'].startswith('arena_team_')
                               for p in candidate.bridge.state()), 'mode assigns an Arena team')
    previous_team = int(next(p for p in candidate.bridge.state() if p['id'] == p2avatar)['teams'].rsplit('_', 1)[1])
    new_team = previous_team % 3 + 1
    third.bridge.call('input', controller=0, hold={'quit': 1.0}, steps=1)
    wait(third, lambda: 0 in status(third)['open_menu_players'], 'remote Arena team menu')
    assert not probe(third, f'LobbySetPlayerTeam({tp["id"]}, {new_team})', target='player_menu', local=0)
    candidate.bridge.call('input', controller=0, hold={'quit': 1.0}, steps=1)
    wait(candidate, lambda: 0 in status(candidate)['open_menu_players'], 'host Arena team menu')
    assert probe(candidate, f'!LobbyGetForceTeams() && LobbySetPlayerTeam({cp2["id"]}, {new_team})', target='player_menu', local=0)
    for inst in live:
        wait(inst, lambda i=inst: any(p['common_id'] == p2avatar and p['teams'] == 'arena_team_' + str(new_team)
                                     for p in i.bridge.state()), 'host regrouping applies to actual Arena avatars')
    code(candidate, 'array<LobbyPlayer>@ ap = LobbyPlayers(); int displayed = -1; '
         f'for(uint k=0;k<ap.length();++k) if(ap[k].id=={cp2["id"]}) displayed=ap[k].team; '
         'SetConfigValueInt("authority_displayed_team", displayed); SaveConfig();', target='player_menu', local=0)
    assert int(config(candidate)['authority_displayed_team']) == new_team
    metrics['natural_arena_host_override'] = {'before': previous_team, 'after': new_team, 'force_lobby_teams': False}
    candidate.bridge.call('exec', target='player_menu', local_player=0, code='this_ui.SendCallback("end_game");')
    for inst in live:
        wait(inst, lambda i=inst: status(i)['online']['lobby_waiting'] and not status(i)['level_loaded'], 'Arena returns migrated lobby', 60)
    print('authority: natural Arena host regrouping PASS', flush=True)
    # Campaign teams remain natural by default, but an explicit host edit must
    # visibly take effect rather than silently reverting the dropdown.
    assert probe(candidate, 'LobbySetSelection("com-wolfire-overgrowth-campaign", "com-wolfire-overgrowth-campaign", '
                 '"Data/Levels/og_story/01_Slaver_Camp_Landing.xml", false) && LobbyStart()')
    for inst in live:
        wait(inst, lambda i=inst: status(i)['level_loaded'] and not status(i)['waiting_for_input'], 'campaign on migrated server', 180)
    campaign_avatar = find(candidate, 'Candidate Two')['avatar_id']
    natural_team = next(p for p in candidate.bridge.state() if p['id'] == campaign_avatar)['teams']
    assert probe(candidate, f'!LobbyGetForceTeams() && LobbySetPlayerTeam({cp2["id"]}, 2)', actual=0, target='level')
    for inst in live:
        wait(inst, lambda i=inst: any(p['common_id'] == campaign_avatar and p['teams'] == 'lobby_team_2'
                                     for p in i.bridge.state()), 'explicit campaign host team applies')
    assert not probe(third, f'LobbySetPlayerTeam({tp["id"]}, 2)', actual=0, target='level')
    assert probe(candidate, f'LobbySetPlayerTeam({cp2["id"]}, 1)', actual=0, target='level')
    for inst in live:
        wait(inst, lambda i=inst: any(p['common_id'] == campaign_avatar and p['teams'] == natural_team
                                     for p in i.bridge.state()), 'campaign Team1 restores natural faction')
    metrics['campaign_host_override'] = {'natural': natural_team, 'changed': 'lobby_team_2', 'restored': natural_team}
    code(candidate, 'LobbyEndGame();', target='level')
    for inst in live:
        wait(inst, lambda i=inst: status(i)['online']['lobby_waiting'] and not status(i)['level_loaded'], 'campaign returns migrated lobby', 60)
    print('authority: campaign host override and natural faction restore PASS', flush=True)
    # A deliberate host departure must close the guest promptly, independently
    # of the transport's timeout for an unreachable machine.
    started = time.monotonic()
    candidate.stop()
    wait(third, lambda: not status(third)['online']['active'], 'known host left, guest can leave', 10)
    metrics['known_host_leave_seconds'] = time.monotonic() - started
    return {'passed': True, 'metrics': metrics, 'notes': []}
