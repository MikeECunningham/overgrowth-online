"""Explicit AI roster seats participate in Arena rounds without local inputs.

Exercises an obsolete autofill config, live additions, forced mixed teams,
team scoring, KO/revival, end/restart identity retention and live removal.
"""
import os
import time

from .scenarios import script_errors


def roster_bots_arena(ctx):
    host = ctx.instance('roster_bots_arena_host', width=1280, height=720,
                        keys={'mp_port': '25844', 'playername': 'Arena human',
                              'oo_arena_bots': '8', 'oo_arena_random': 'false'})
    metrics = {}

    def code(text, target='level', arena=False):
        return host.bridge.call('exec', target=target, code=text,
                                **({'context': 'level_specific'} if arena else {}))

    def status():
        return host.bridge.call('status', timeout=240)

    def rows():
        return status()['online']['players']

    def wait(predicate, label, seconds=40):
        return host.bridge.wait(predicate, seconds, step=0.1, what=label)

    def probe(expression):
        host.bridge.call('level_messages', prefix='bot_arena_probe')
        code('level.SendMessage("bot_arena_probe " + (' + expression + '));', arena=True)
        messages = host.bridge.call('level_messages', prefix='bot_arena_probe')['messages']
        assert messages, 'Arena probe did not execute: ' + expression
        return messages[-1].split(' ', 1)[1]

    def fight(count):
        wait(lambda: probe('"" + int(phase) + " " + roster.length()') == f'2 {count}',
             'Arena fighting with expected roster')
        code('max_points=3; for(uint k=0;k<bots.length();++k) '
             'ReadCharacterID(bots[k]).ReceiveScriptMessage("set_combat_allowed false");', arena=True)

    def ready():
        def loaded():
            value = status()
            if not value['level_loaded']:
                return False
            if value['waiting_for_input']:
                host.bridge.call('start_session')
                return False
            return True
        wait(loaded, 'Arena world started', 240)

    def bot_rows():
        return [p for p in rows() if p['bot']]

    def bot_actors():
        ids = {p['avatar_id'] for p in bot_rows()}
        return [c for c in host.bridge.state() if c['id'] in ids]

    def assert_no_bot_inputs():
        value = status()
        assert value['local_players'] == 1 and value['local_inputs'] == 1, value
        assert value['render_views'] == 1, value
        assert all(not p['controlled'] and not p['is_player'] for p in bot_actors()), bot_actors()

    try:
        code('LobbyBeginHost(); LobbySetForceTeams(true); '
             'LobbySetSelection("com-wolfire-arena","com-wolfire-arena",'
             '"Data/Levels/arenas/Magma_Arena.xml",false); LobbyStart();', target='menu')
        ready()
        time.sleep(4)
        assert len(rows()) == 1 and len(host.bridge.state()) == 1, 'legacy config created phantom bots'
        assert probe('"" + int(phase)') == '0', 'solo Arena should wait for an opponent'
        metrics['no_legacy_autofill'] = status()

        code('LobbyAddBot();')
        wait(lambda: len(bot_rows()) == 1 and bot_rows()[0]['avatar_id'] >= 0, 'first explicit AI actor')
        first = bot_rows()[0]
        code(f'LobbySetPlayerName({first["id"]},"Arena opponent"); LobbySetPlayerTeam({first["id"]},2);')
        fight(2)
        assert_no_bot_inputs()
        print('Arena explicit human+bot fight started', flush=True)
        metrics['human_and_bot_fight'] = {'status': status(), 'actors': host.bridge.state()}

        code('LobbyAddBot();')
        wait(lambda: len(bot_rows()) == 2 and all(p['avatar_id'] >= 0 for p in bot_rows()), 'late AI actor')
        late = next(p for p in bot_rows() if p['id'] != first['id'])
        code(f'LobbySetPlayerName({late["id"]},"Late teammate"); LobbySetPlayerTeam({late["id"]},2);')
        assert probe('"" + roster.length()') == '2', 'late bot entered the active round'
        assert probe(f'"" + ReadCharacterID({late["avatar_id"]}).GetBoolVar("combat_allowed")') == 'false'
        metrics['late_waits_for_round'] = {'rows': rows(), 'arena': probe('"" + roster.length() + " " + bots.length()')}
        code('NewMatch();', arena=True)
        fight(3)
        actors = {c['id']: c for c in host.bridge.state()}
        assert actors[first['avatar_id']]['teams'] == actors[late['avatar_id']]['teams'] == 'arena_team_2'
        human = next(p for p in rows() if not p['bot'])
        assert actors[human['avatar_id']]['teams'] == 'arena_team_1'
        code('SetKnockedOut(_unconscious); Ragdoll(_RGDL_LIMP);', target=first['avatar_id'])
        time.sleep(0.3)
        assert probe('"" + int(phase)') == '2', 'round ended while a bot teammate remained standing'
        code('SetKnockedOut(_unconscious); Ragdoll(_RGDL_LIMP);', target=late['avatar_id'])
        wait(lambda: probe('"" + int(phase)') == '3', 'human team earns round point')
        assert probe(f'"" + points[roster.find({human["avatar_id"]})]') == '1'
        assert probe(f'"" + points[roster.find({first["avatar_id"]})] + " " + points[roster.find({late["avatar_id"]})]') == '0 0'
        fight(3)
        assert all(c['knocked_out'] == 0 for c in bot_actors()), 'round reset failed to revive the same bot actors'
        assert {p['id']: p['avatar_id'] for p in bot_rows()} == {first['id']: first['avatar_id'], late['id']: late['avatar_id']}
        metrics['scoring_and_revival'] = {'rows': rows(), 'actors': host.bridge.state()}
        print('Arena late bot, team score and same-actor round revival passed', flush=True)

        code('SetConfigValueBool("oo_arena_random",true); NewMatch();', arena=True)
        fight(3)
        random_actors = {c['id']: c for c in host.bridge.state()}
        for bot in bot_rows():
            expected = probe(f'rnd_paths[roster.find({bot["avatar_id"]})]')
            assert random_actors[bot['avatar_id']]['char_path'] == expected, ('bot randomizer did not switch its host-simulated body', bot, expected)
        assert_no_bot_inputs()
        metrics['randomized'] = {'rows': rows(), 'actors': list(random_actors.values())}
        code('SetConfigValueBool("oo_arena_random",false); NewMatch();', arena=True)
        fight(3)
        restored = {c['id']: c for c in host.bridge.state()}
        assert restored[human['avatar_id']]['char_path'] == 'Data/Characters/turner.xml'
        for bot in bot_rows():
            assert restored[bot['avatar_id']]['char_path'] == random_actors[bot['avatar_id']]['char_path']

        identity = {p['id']: (p['name'], p['team']) for p in bot_rows()}
        code('LobbyEndGame();')
        wait(lambda: status()['online']['lobby_waiting'] and not status()['level_loaded'], 'Arena returns to lobby')
        assert {p['id']: (p['name'], p['team']) for p in bot_rows()} == identity
        code('LobbyStart();', target='menu')
        ready()
        fight(3)
        assert {p['id']: (p['name'], p['team']) for p in bot_rows()} == identity
        assert_no_bot_inputs()
        metrics['restart'] = {'rows': rows(), 'actors': host.bridge.state()}
        print('Arena randomized bots and retained lobby restart passed', flush=True)
        image = os.path.abspath(os.path.join(ctx.out_dir, 'roster_bots_arena.png'))
        host.bridge.call('screenshot', path=image)
        metrics['screenshot'] = image

        removed = [p['avatar_id'] for p in bot_rows()]
        code(''.join(f'LobbyRemovePlayer({p["id"]});' for p in bot_rows()))
        wait(lambda: not bot_rows() and all(c['id'] not in removed for c in host.bridge.state()), 'native removes bot actors')
        wait(lambda: probe('"" + int(phase) + " " + roster.length()') == '0 0', 'Arena returns to waiting without opponents')
        time.sleep(4)
        assert len(rows()) == 1 and len(host.bridge.state()) == 1, 'removed bot reappeared from obsolete autofill'
        assert_no_bot_inputs()
        metrics['removed_without_autofill'] = status()
        errors = script_errors(host)
        assert not errors, errors
        return {'passed': True, 'metrics': metrics, 'notes': []}
    finally:
        host.stop()
