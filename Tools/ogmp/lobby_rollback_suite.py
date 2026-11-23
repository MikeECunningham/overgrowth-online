"""A candidate closes after commit while the original server is rejoining.
The original listener and surviving local-player identities must return promptly.
"""
import time

from .scenarios import LEVEL


def lobby_rollback(ctx):
    host = ctx.instance('rollback_host', keys={'mp_port': '25844', 'playername': 'Original'})
    candidate = ctx.instance('rollback_candidate', keys={'mp_port': '25845', 'playername': 'Candidate'})
    survivor = ctx.instance('rollback_survivor', keys={'mp_port': '25846', 'playername': 'Survivor'})
    metrics = {}

    def code(inst, text):
        return inst.bridge.call('exec', target='menu', code=text)

    def status(inst):
        return inst.bridge.status()['online']

    def rows(inst):
        return status(inst)['players']

    code(host, 'LobbyBeginHost(); LobbyAddLocalPlayer(); '
         f'LobbySetSelection("sandbox","","Data/Levels/{LEVEL}",false);')
    for inst in (candidate, survivor):
        inst.bridge.call('connect', ip='127.0.0.1:25844')
    host.bridge.wait(lambda: len(rows(host)) == 4, 30, what='four original lobby seats')
    original = rows(host)
    target = next(p for p in original if p['name'] == 'Candidate')
    p2 = next(p for p in original if p['peer_id'] == 255 and p['local_index'] == 1)
    code(host, f'LobbySetPlayerName({p2["id"]},"Original P2"); '
         f'LobbySetPlayerScreen({p2["id"]},1); LobbySetPlayerTeam({p2["id"]},2);')
    original = {p['id']: {k: p[k] for k in ('name', 'team', 'screen', 'local_index', 'leader', 'host')}
                for p in rows(host) if p['peer_id'] != target['peer_id']}

    # The reachability probes bypass gameplay simulation. Delaying the new
    # server's handshake gives a deterministic window after the old listener
    # has stopped but before the original server finishes admission.
    candidate.bridge.call('net_sim', latency_ms=1500, jitter_ms=0, loss=0)
    code(host, f'LobbyPromoteHost({target["id"]});')
    host.bridge.wait(lambda: status(host)['client'] and status(host)['migration_busy'] and
                     'Rejoining' in status(host)['migration_status'],
                     30, step=0.01, what='original server becomes joining client')
    assert status(candidate)['hosting'], 'candidate never opened its new game server'
    metrics['failure_window'] = {'host': status(host), 'candidate': status(candidate), 'survivor': status(survivor)}
    t0 = time.monotonic()
    candidate.bridge.call('disconnect')  # sends a real transport close to both joining clients
    for inst in (host, survivor):
        inst.bridge.wait(lambda i=inst: status(i)['active'] and not status(i)['migration_busy'] and
                         len(rows(i)) == 3 and status(i)['decision_host'] == 255,
                         12, step=0.02, what='original lobby restored without waiting for failed candidate')
    metrics['restore_seconds'] = time.monotonic() - t0
    assert status(host)['hosting'] and status(survivor)['client']
    assert not status(candidate)['active']
    for inst in (host, survivor):
        restored = {p['id']: {k: p[k] for k in ('name', 'team', 'screen', 'local_index', 'leader', 'host')}
                    for p in rows(inst)}
        assert restored == original, (inst.name, original, restored)
    metrics['restored_host'] = status(host)
    metrics['restored_survivor'] = status(survivor)
    # Authority and the start action must work immediately after rollback.
    code(host, 'LobbyStart();')
    assert not status(host)['lobby_waiting'], 'rollback still blocks the host from starting'
    for inst in (host, survivor):
        inst.bridge.wait(lambda i=inst: (lambda s: s['level_loaded'] and not s['waiting_for_input'])(
            i.bridge.call('status', timeout=180)), 180, what='match starts on restored server')
    assert host.bridge.status()['local_players'] == 2
    assert len([c for c in host.bridge.state() if c['is_player']]) == 3
    return {'passed': True, 'metrics': metrics, 'notes': []}
