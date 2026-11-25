"""Explicit bot seats survive a real server handoff without becoming local users."""
from pathlib import Path
import shutil
import tempfile
import xml.etree.ElementTree as ET


def bot_migration(ctx):
    # Save-only mod data belongs to the current host, including after promotion.
    with tempfile.TemporaryDirectory() as temporary:
        mod = Path(temporary, 'session-rules')
        shutil.copytree(Path(__file__).resolve().parents[2] / 'test/mods/session-rules', mod)
        manifest = ET.parse(mod / 'mod.xml')
        contract = manifest.getroot().find('Session')
        contract.attrib.pop('network_version')
        contract.remove(contract.find('LobbySetting'))
        manifest.write(mod / 'mod.xml', encoding='utf-8', xml_declaration=True)
        h = ctx.instance('bot_migration_host', mods=[str(mod)], keys={'mp_port': '27744', 'playername': 'Original human'})
        c = ctx.instance('bot_migration_candidate', mods=[str(mod)], keys={'mp_port': '27745', 'playername': 'New host human'})
    machines = [h, c]
    metrics = {}
    serial = 0

    def status(i):
        return i.bridge.status()

    def rows(i):
        return status(i)['online']['players']

    def code(i, text, target='menu'):
        return i.bridge.call('exec', target=target, code=text)

    def wait(i, predicate, description, timeout=40):
        return i.bridge.wait(predicate, timeout, what=description)

    def probe(i, expression, target='menu'):
        nonlocal serial
        serial += 1
        code(i, f'SetConfigValueInt("bot_migration_probe_serial",{serial}); '
                f'SetConfigValueBool("bot_migration_probe",({expression})); SaveConfig();', target)
        config = dict(line.split(': ', 1) for line in (Path(i.writedir) / 'Data/config.txt').read_text().splitlines()
                      if ': ' in line)
        assert int(config['bot_migration_probe_serial']) == serial, 'script probe did not execute'
        return config['bot_migration_probe'] in ('1', 'true')

    assert probe(h, 'LobbyBeginHost() && LobbyAddBot() && LobbyAddBot()')
    secret = r'"{\"host_seat\":255}"'
    assert probe(h, f'SessionSetModData("session-rules",{secret})')
    bots = sorted([p for p in rows(h) if p['bot']], key=lambda p: p['id'])
    assert len(bots) == 2
    for bot, name, team in zip(bots, ['North Bot', 'South Bot'], [2, 3]):
        assert probe(h, f'LobbySetPlayerName({bot["id"]},"{name}") && LobbySetPlayerTeam({bot["id"]},{team})')
        assert not probe(h, f'LobbyCanPromoteHost({bot["id"]}) || LobbyCanPromoteLeader({bot["id"]}) || LobbySetPlayerScreen({bot["id"]},1)')
    assert probe(h, 'LobbySetHostControlsTeams(false) && LobbySetSelection("sandbox","","Data/Levels/Project60/4_red_hills.xml",false)')
    c.bridge.call('connect', ip='127.0.0.1:27744')
    for i in machines:
        wait(i, lambda i=i: len(rows(i)) == 4, 'two humans and two bot seats')
        assert status(i)['local_inputs'] == 1 and status(i)['local_players'] == 1
        for bot in [p for p in rows(i) if p['bot']]:
            assert bot['peer_id'] == 255 and bot['screen'] == 0
            assert not bot['host'] and not bot['leader']
    assert probe(c, 'SessionGetModData("session-rules") == "{}"')
    first = bots[0]['id']
    assert not probe(c, 'LobbyAddBot()')
    assert not probe(c, f'LobbySetPlayerName({first},"Stolen") || LobbySetPlayerTeam({first},1) || LobbyRemovePlayer({first})')
    before = {p['id']: (p['name'], p['team']) for p in rows(h) if p['bot']}
    target = next(p for p in rows(h) if p['name'] == 'New host human')['id']
    assert probe(h, f'LobbyPromoteHost({target})')
    for i in machines:
        wait(i, lambda i=i: not status(i)['online']['migration_busy'] and
             status(i)['online']['decision_host'] == target and len(rows(i)) == 4,
             'actual host transfer with bots', 70)
        assert {p['id']: (p['name'], p['team']) for p in rows(i) if p['bot']} == before
        assert status(i)['local_inputs'] == 1 and status(i)['local_players'] == 1
    assert status(c)['online']['hosting'] and status(h)['online']['client']
    assert probe(h, 'Online_GetOwnPlayerID() == 255 && Online_GetPlayerName(255) == "Original human"')
    assert probe(c, f'SessionGetModData("session-rules") == {secret}')
    assert probe(h, 'SessionGetModData("session-rules") == "{}"')
    metrics['save_only_data_transferred'] = True
    metrics['migrated_roster'] = rows(c)
    print('bot_migration: actual transfer and human-only admission PASS', flush=True)
    h.stop()
    wait(c, lambda: len(rows(c)) == 3 and not status(c)['online']['peers'], 'original host leaves while bot seats stay')
    assert {p['id']: (p['name'], p['team']) for p in rows(c) if p['bot']} == before
    assert probe(c, 'LobbyCanStart() && LobbyStart()')
    wait(c, lambda: status(c)['level_loaded'] and not status(c)['waiting_for_input'], 'new server starts with bots', 180)
    wait(c, lambda: all(p['avatar_id'] >= 0 for p in rows(c)), 'all bot avatars assigned')
    assert probe(c, 'LobbySetPaused(true)', target='level')
    actors = {p['id']: p for p in c.bridge.state()}
    for bot in [p for p in rows(c) if p['bot']]:
        actor = actors[bot['avatar_id']]
        assert not actor['is_player'] and not actor['controlled'] and not actor['proxy'], actor
        assert actor['controller_id'] == -1 and actor['camera_id'] == 0, actor
        assert actor['teams'] == 'lobby_team_' + str(bot['team']), actor
    st = status(c)
    assert st['local_inputs'] == 1 and st['local_players'] == 1 and st['render_views'] == 1, st
    assert len([a for a in actors.values() if a['controlled']]) == 1
    metrics['started_roster'] = rows(c)
    metrics['bot_actors'] = [actors[p['avatar_id']] for p in rows(c) if p['bot']]
    assert probe(c, 'LobbyEndGame()', target='level')
    wait(c, lambda: status(c)['online']['lobby_waiting'] and not status(c)['level_loaded'], 'bots persist after End game', 60)
    assert {p['id']: (p['name'], p['team']) for p in rows(c) if p['bot']} == before
    metrics['returned_roster'] = rows(c)
    return {'passed': True, 'metrics': metrics, 'notes': []}
