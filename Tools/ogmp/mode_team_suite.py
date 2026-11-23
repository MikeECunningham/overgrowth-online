"""Mode-owned arena teams, optional lobby override, and natural campaign allies.

Runs host2/client2 plus two AI fighters, exercises authored 3v3 and six-way FFA,
two team-scoring rounds, manual override, and both campaign team policies.
"""
import collections
import math
import time
import traceback

def mode_teams(ctx):
    h = ctx.instance('mode_team_host', width=1280, height=720, keys={'mp_port': '26844', 'playername': 'Host', 'gl_debug': '1'})
    c = ctx.instance('mode_team_guest', width=1280, height=720, keys={'mp_port': '26844', 'playername': 'Guest', 'gl_debug': '1'})
    r = {}

    def code(i, s, target='level'):
        return i.bridge.call('exec', target=target, code=s)

    def acode(i, s):
        return i.bridge.call('exec', target='level', context='level_specific', code=s)

    def st(i):
        return i.bridge.status()

    def rows(i):
        return st(i)['online'].get('players', [])

    def wait(i, p, d, t=30):
        return i.bridge.wait(p, t, what=d)

    def probe(i, expr):
        i.bridge.call('level_messages')
        acode(i, 'level.SendMessage("teamprobe " + (' + expr + '));')
        time.sleep(0.08)
        lines = i.bridge.call('level_messages', prefix='teamprobe')['messages']
        return lines[-1].split(' ', 1)[1] if lines else None

    def arena(i=h):
        return probe(i, '"" + int(phase) + " " + round_number + " " + roster.length() + " " + battle_name')

    def points(i=h):
        field = 'points' if i is h else 'score_points'
        return probe(i, '""' + ''.join(' + " " + ' + field + '[' + str(k) + ']' for k in range(6))).strip()

    def menu_input_mask():
        client = {p['controller_id']: p for p in c.bridge.state() if not p['proxy'] and p['controlled']}
        remote = next(p for p in h.bridge.state() if p['common_id'] == client[1]['common_id'])

        def host_pressed():
            h.bridge.call('level_messages')
            code(h, 'level.SendMessage("inputprobe " + GetInputDown(' + str(remote['controller_id']) + ', "up"));')
            time.sleep(0.08)
            messages = h.bridge.call('level_messages', prefix='inputprobe')['messages']
            return messages[-1].split()[-1] if messages else None

        c.bridge.call('input', controller=1, hold={'up': 1.0}, steps=-1)
        wait(h, lambda: host_pressed() == 'true', 'remote P2 input arrives before menu')
        c.bridge.call('input', controller=1, release_all=True)
        wait(h, lambda: host_pressed() == 'false', 'remote P2 input releases')
        c.bridge.call('input', controller=1, hold={'quit': 1.0}, steps=1)
        wait(c, lambda: st(c).get('open_menu_players') == [1], 'only client P2 menu is open')
        time.sleep(0.2)
        before = {p['controller_id']: p['pos'] for p in c.bridge.state() if not p['proxy'] and p['controlled']}
        for local in [0, 1]:
            c.bridge.call('input', controller=local, hold={'up': 1.0}, steps=-1)
        time.sleep(0.8)
        assert host_pressed() == 'false', 'P2 menu navigation leaked into host gameplay input'
        for local in [0, 1]:
            c.bridge.call('input', controller=local, release_all=True)
        time.sleep(0.2)
        after = {p['controller_id']: p['pos'] for p in c.bridge.state() if not p['proxy'] and p['controlled']}
        r['menu_input_mask'] = {'p1_moved_m': math.dist(before[0], after[0]),
                                'p2_moved_m': math.dist(before[1], after[1])}
        assert r['menu_input_mask']['p1_moved_m'] > 1
        assert r['menu_input_mask']['p2_moved_m'] < 0.75
        c.bridge.call('exec', target='player_menu', local_player=1, code='this_ui.SendCallback("close");')
        wait(c, lambda: not st(c).get('open_menu_players'), 'P2 closes its menu')

    def end():
        for i in [h, c]:
            for local in [0, 1]:
                i.bridge.call('input', controller=local, hold={'quit': 1.0}, steps=1)
                wait(i, lambda i=i, local=local: local in st(i).get('open_menu_players', []), 'personal menu opened', 10)
        h.bridge.call('exec', target='player_menu', local_player=0, code='this_ui.SendCallback("end_game");')
        for i in [h, c]:
            wait(i, lambda i=i: st(i)['online']['lobby_waiting'] and (not st(i)['level_loaded']), 'return lobby', 60)
            assert not st(i).get('open_menu_players'), st(i)

    def start():
        code(h, 'LobbyStart();', 'menu')
        for i in [h, c]:
            wait(i, lambda i=i: st(i)['level_loaded'] and (not st(i)['waiting_for_input']), 'loaded/started', 180)

    def matched():
        a = {p['common_id']: (p['teams'], p['palette']) for p in h.bridge.state()}
        b = {p['common_id']: (p['teams'], p['palette']) for p in c.bridge.state()}
        return a == b

    def KO(common):
        own = next((p for p in c.bridge.state() if p['common_id'] == common and (not p['proxy'])), None)
        machine = c if own else h
        p = own or next((p for p in h.bridge.state() if p['common_id'] == common))
        code(machine, 'SetKnockedOut(_unconscious); Ragdoll(_RGDL_LIMP);', p['id'])
    try:
        code(h, 'LobbyBeginHost(); LobbyAddLocalPlayer(); LobbyAddBot(); LobbyAddBot();', 'menu')
        code(c, 'LobbyAddLocalPlayer();', 'menu')
        c.bridge.call('connect', ip='127.0.0.1')
        for i in [h, c]:
            wait(i, lambda i=i: len(rows(i)) == 6, 'four humans and two explicit bots')
        assert sum(p['bot'] for p in rows(h)) == 2
        code(h, ''.join(f"LobbySetPlayerTeam({p['id']},1);" for p in rows(h)), 'menu')
        manual = {p['id']: p['team'] for p in rows(h)}
        assert set(manual.values()) == {1}
        code(c, 'LobbySetForceTeams(true);', 'menu')
        code(h, 'LobbySetSelection("com-wolfire-arena","com-wolfire-arena","Data/Levels/arenas/Magma_Arena.xml",false);', 'menu')
        start()
        wait(h, lambda: len(h.bridge.state()) == 6, 'four humans/two bots')
        wait(h, lambda: arena() is not None and len({p['teams'] for p in h.bridge.state()}) >= 2, 'Arena teams', 40)
        r['initial_arena'] = arena()
        r['initial_characters'] = {'host': h.bridge.state(), 'guest': c.bridge.state()}
        print('Arena compiled', r['initial_arena'], flush=True)
        for attempt in range(15):
            values = h.bridge.state()
            counts = collections.Counter((p['teams'] for p in values))
            if len(counts) == 2 and sorted(counts.values()) == [3, 3]:
                break
            acode(h, 'NewMatch();')
            time.sleep(0.8)
        else:
            raise AssertionError('no grouped authored template selected')
        wait(h, lambda: arena().split()[0] == '2', 'first fighting phase', 20)
        acode(h, 'max_points=2; for(uint k=0;k<bots.length();++k) ReadCharacterID(bots[k]).ReceiveScriptMessage("set_combat_allowed false");')
        wait(c, matched, 'matching actualteams and palettes', 10)
        grouped = h.bridge.state()
        r['grouped'] = {'host': grouped, 'guest': c.bridge.state(), 'arena': arena()}
        winner = sorted({p['teams'] for p in grouped})[0]
        target = [p['common_id'] for p in grouped if p['teams'] != winner]
        allies = [p['common_id'] for p in grouped if p['teams'] == winner]
        KO(allies[0])
        for common in target:
            KO(common)
        wait(h, lambda: arena().split()[0] == '3', 'first team round awarded', 12)
        r['round1_host_points'] = points()
        r['round1_client_points'] = points(c)
        r['round1_arena'] = arena()
        assert r['round1_host_points'] == r['round1_client_points']
        assert sorted(map(int, r['round1_host_points'].split())) == [0, 0, 0, 1, 1, 1]
        wait(h, lambda: arena().split()[:2] == ['2', '2'], 'second fighting round', 20)
        acode(h, 'for(uint k=0;k<bots.length();++k) ReadCharacterID(bots[k]).ReceiveScriptMessage("set_combat_allowed false");')
        for common in target:
            KO(common)
        wait(h, lambda: arena().split()[0] == '3', 'second team round awarded', 12)
        r['round2_host_points'] = points()
        r['round2_client_points'] = points(c)
        assert r['round2_host_points'] == r['round2_client_points']
        assert sorted(map(int, r['round2_host_points'].split())) == [0, 0, 0, 2, 2, 2]
        for attempt in range(15):
            acode(h, 'NewMatch();')
            time.sleep(0.7)
            if len({p['teams'] for p in h.bridge.state()}) == 6:
                break
        else:
            raise AssertionError('no FFA template selected')
        wait(c, matched, 'six-way FFA replicated', 10)
        r['ffa'] = {'host': h.bridge.state(), 'guest': c.bridge.state(), 'arena': arena()}
        assert {p['id']: p['team'] for p in rows(h)} == manual, 'mode changed manual lobby plan'
        menu_input_mask()
        end()
        code(h, ''.join(f"LobbyRemovePlayer({p['id']});" for p in rows(h) if p['bot']), 'menu')
        for i in [h, c]:
            wait(i, lambda i=i: len(rows(i)) == 4, 'human-only team-override phase')
        hp = [p for p in rows(h) if p['peer_id'] == 255]
        gp = [p for p in rows(h) if p['peer_id'] != 255]
        code(h, 'LobbySetForceTeams(true);' + ''.join((f"LobbySetPlayerTeam({p['id']},{(1 if p['peer_id'] == 255 else 2)});" for p in rows(h))), 'menu')
        code(c, 'LobbySetForceTeams(false);', 'menu')
        start()
        wait(h, lambda: len({p['teams'] for p in h.bridge.state()}) == 2, 'forced two teams', 40)
        wait(h, lambda: all(p['teams'].startswith('arena_team_') for p in h.bridge.state()),
             'forced match publishes its effective teams', 40)
        assert sorted(collections.Counter((p['teams'] for p in h.bridge.state())).values()) == [2, 2]
        wait(c, matched, 'forced palettes replicated', 10)
        r['forced_arena'] = {'host': h.bridge.state(), 'guest': c.bridge.state()}
        assert probe(c, '"" + LobbyGetForceTeams()') == 'true'
        forced = {p['id']: p for p in h.bridge.state()}
        for player in rows(h):
            assert forced[player['avatar_id']]['teams'] == 'arena_team_' + str(player['team'])
        end()
        ordered = rows(h)
        code(h, 'LobbySetForceTeams(false); LobbySetSelection("com-wolfire-overgrowth-campaign","com-wolfire-overgrowth-campaign","Data/Levels/og_story/01_Slaver_Camp_Landing.xml",false);' + ''.join((f"LobbySetPlayerTeam({p['id']},{k + 1});" for k, p in enumerate(ordered))), 'menu')
        start()
        time.sleep(0.5)
        ids = {p['avatar_id'] for p in rows(h)}
        humans = [p for p in h.bridge.state() if p['id'] in ids]
        r['coop_natural'] = {'players': rows(h), 'humans': humans}
        assert len({p['teams'] for p in humans}) == 1, r['coop_natural']
        expected = {p['common_id']: p['teams'] for p in humans}
        assert {p['common_id']: p['teams'] for p in c.bridge.state() if p['common_id'] in ids} == expected
        end()
        code(h, 'LobbySetForceTeams(true);' + ''.join((f"LobbySetPlayerTeam({p['id']},{(2 if k == 0 else 3 if k == 2 else 1)});" for k, p in enumerate(rows(h)))), 'menu')
        start()
        time.sleep(0.5)
        ids = {p['avatar_id'] for p in rows(h)}
        humans = {p['id']: p for p in h.bridge.state() if p['id'] in ids}
        r['coop_forced'] = {'players': rows(h), 'humans': list(humans.values())}
        tags = {p['id']: humans[p['avatar_id']]['teams'] for p in rows(h)}
        assert tags[rows(h)[0]['id']] == 'lobby_team_2'
        assert tags[rows(h)[1]['id']] == tags[rows(h)[3]['id']] and tags[rows(h)[1]['id']] != 'lobby_team_2'
        assert tags[rows(h)[2]['id']] == 'lobby_team_3'
        expected = {p['common_id']: p['teams'] for p in humans.values()}
        assert {p['common_id']: p['teams'] for p in c.bridge.state() if p['common_id'] in ids} == expected
        r['passed'] = True
    except Exception:
        return {'passed': False, 'metrics': r, 'notes': [traceback.format_exc()]}
    return {'passed': True, 'metrics': r, 'notes': []}
