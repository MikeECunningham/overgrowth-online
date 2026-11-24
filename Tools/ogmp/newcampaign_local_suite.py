"""Campaign parties use real story IDs, authored dialogue, and SDL virtual pads.

newcampaign_local: offline White Flags -> Slaver Path; private local party,
controller/settings/name/screen persistence; open that party to a late remote
and its second local during Civ1, then finish the campaign level again.
newcampaign_roles: four humans start through the lobby with host/leader P2;
the authored P1 remains the story actor and all roles survive levelwin.
newcampaign_main_removal: original P1 leaves; surviving actual input1 owns the
authored hero, advances dialogue, and identifies Civ1's nearby authored NPC363.
newcampaign_cinematic_retry: retry while four humans are in authored dialogue;
fresh actors must not inherit old object-ID hold, visibility or health state.
All input is process-local through the opt-in bridge, never the desktop.
"""
import math
import os
import time

from .scenarios import script_errors

CAMPAIGN = 'com-wolfire-overgrowth-campaign'
FIRST_ID, SECOND_ID = 'river_village_intro', 'slaver_camp'
FIRST = 'og_story/01_Slaver_Camp_Landing.xml'
SECOND = 'og_story/02_Slaver_Camp.xml'


def _code(inst, text, target='level'):
    return inst.bridge.call('exec', target=target, code=text)


def _status(inst):
    return inst.bridge.call('status', timeout=240)


def _wait(inst, predicate, label, seconds=40):
    return inst.bridge.wait(predicate, seconds, step=0.1, what=label)


def _ready(inst, level=None):
    def ready():
        status = _status(inst)
        if not status['level_loaded'] or (level is not None and level not in status['level']): return False
        if status['waiting_for_input'] and status['online']['hosting']:
            inst.bridge.call('start_session')  # the host's normal post-load ready action
            return False
        return not status['waiting_for_input']
    return _wait(inst, ready, 'campaign world ready', 240)


def _probe(inst, expression):
    inst.bridge.call('level_messages', prefix='campaign_party_probe')
    _code(inst, 'level.SendMessage("campaign_party_probe " + (' + expression + '));')
    messages = inst.bridge.call('level_messages', prefix='campaign_party_probe')['messages']
    assert messages, 'campaign script probe did not execute: ' + expression
    return messages[-1].split(' ', 1)[1]


def _rows(inst):
    return _status(inst)['online']['players']


def _human_chars(inst):
    avatars = {p['avatar_id'] for p in _rows(inst)}
    return {c['id']: c for c in inst.bridge.state() if c['id'] in avatars}


def _signature(inst):
    return {p['id']: {k: p[k] for k in ('name', 'team', 'screen', 'local_index', 'host', 'leader', 'peer_id')}
            for p in _rows(inst)}


def _owned(inst):
    return {c['controller_id']: c for c in _human_chars(inst).values() if not c['remote']}


def _controls(inst, profile):
    return _probe(inst, f'GetBindingValue("gamepad_{profile}","jump") + "|" + '
                  f'GetConfigValueFloat("gamepad_{profile}_look_sensitivity") + "|" + '
                  f'GetConfigValueBool("gamepad_{profile}_invert_x_look") + "|" + '
                  f'GetConfigValueBool("gamepad_{profile}_invert_y_look")')


def _pad_map(inst):
    return {p['slot']: {k: p.get(k) for k in ('instance_id', 'input_id', 'local_index', 'sensitivity')}
            for p in inst.bridge.call('virtual_pad')['pads']}


def _join_pad(inst, slot, count):
    inst.bridge.call('virtual_pad', action='attach', slot=slot)
    time.sleep(0.15)
    inst.bridge.call('virtual_pad', action='button', slot=slot, button=0, down=True)
    time.sleep(0.12)
    inst.bridge.call('virtual_pad', action='button', slot=slot, button=0, down=False)
    _wait(inst, lambda: _status(inst)['local_players'] == count, 'controller joins the running campaign')
    return _pad_map(inst)


def _skip(inst, controller=0):
    # This is the player's normal skip input, including chained intro dialogues.
    for _ in range(12):
        if not _status(inst)['dialogue_camera_control']:
            time.sleep(0.15)
            if not _status(inst)['dialogue_camera_control']:
                return
        inst.bridge.call('input', controller=controller, hold={'skip_dialogue': 1}, steps=10)
        time.sleep(0.45)
    raise AssertionError('authored dialogue did not end on normal skip input')


def _shot(ctx, inst, name):
    path = os.path.abspath(os.path.join(ctx.out_dir, name + '.png'))
    inst.bridge.call('screenshot', path=path)
    return path


def _assert_cutscene(inst, require_script=True):
    status = _status(inst)
    assert status['coop_cutscene'], status
    if require_script: assert status['dialogue_camera_control'], 'authored dialogue has not synchronized yet'
    assert status['render_views'] == 1, 'dialogue still renders a split screen'
    chars = _human_chars(inst)
    main = status['campaign_main_avatar']
    assert main in chars and chars[main]['visible'], 'authored story actor was hidden'
    for actor in chars.values():
        if actor['id'] != main:
            assert not actor['visible'], ('extra human visible in dialogue', actor)
            if not actor['remote']:
                assert actor.get('coop_cutscene_hold'), ('local extra not held', actor)
    return chars


def _advance(host, peers, expected):
    _code(host, 'SendGlobalMessage("levelwin");')
    for inst in peers:
        _ready(inst, SECOND)
    for inst in peers:
        _wait(inst, lambda i=inst: len(_rows(i)) == expected, 'party roster after authored levelwin')


def newcampaign_local(ctx):
    host = ctx.instance('campaign_local_host', width=1280, height=720,
                        keys={'mp_port': '25844', 'playername': 'Campaign keyboard',
                              'gamepad_1_look_sensitivity': '2.75', 'gamepad_1[jump]': 'b',
                              'gamepad_1_invert_x_look': 'true', 'gamepad_1_invert_y_look': 'false'})
    guest = None
    metrics = {}
    try:
        host.bridge.call('load_level', path='set_campaign ' + CAMPAIGN)
        host.bridge.call('load_level', path='load_campaign_level ' + FIRST_ID)
        _ready(host, FIRST)
        assert not _status(host)['online']['active'], 'fixture did not start as offline single player'
        _wait(host, lambda: _status(host)['dialogue_camera_control'], 'real opening dialogue')
        metrics['initial_offline'] = _status(host)
        _join_pad(host, 0, 2)
        _wait(host, lambda: _status(host)['online']['local_only'] and _status(host)['coop_cutscene'],
              'offline join promotes to private party')
        before = _assert_cutscene(host)
        extra = next(c for c in before.values() if c['id'] != _status(host)['campaign_main_avatar'])
        host.bridge.call('virtual_pad', action='axis', slot=0, axis=1, value=-32768)
        time.sleep(0.8)
        host.bridge.call('virtual_pad', action='axis', slot=0, axis=1, value=0)
        after = _assert_cutscene(host)
        assert math.dist(extra['pos'], after[extra['id']]['pos']) < 0.2, 'new local moved during opening dialogue'
        metrics['opening_dialogue'] = {'status': _status(host), 'characters': after,
                                      'screenshot': _shot(ctx, host, 'campaign_opening_local_join')}
        _skip(host)
        _wait(host, lambda: not _status(host)['coop_cutscene'] and _status(host)['render_views'] == 2,
              'opening returns to two local views')
        p2 = next(p for p in _rows(host) if p['local_index'] == 1)
        assert _probe(host, f'LobbySetPlayerName({p2["id"]},"Campaign pad")') == 'true'
        assert _probe(host, f'LobbySetPlayerScreen({p2["id"]},1)') == 'true'
        identity = _signature(host)
        controls, pads = _controls(host, 1), _pad_map(host)
        assert controls.split('|')[0] == 'b' and abs(float(controls.split('|')[1]) - 2.75) < 0.01
        assert pads[0]['local_index'] == 1 and abs(pads[0]['sensitivity'] - 2.75) < 0.01, pads
        _advance(host, [host], 2)
        _skip(host)
        assert _signature(host) == identity, 'offline campaign progression changed party identities'
        assert _controls(host, 1) == controls and _pad_map(host) == pads, 'controller/profile changed between levels'
        _wait(host, lambda: len(_owned(host)) == 2 and _status(host)['render_views'] == 2, 'two local bodies/views on level two')
        start = _owned(host)
        actual = pads[0]['input_id']
        host.bridge.call('virtual_pad', action='axis', slot=0, axis=1, value=-32768)
        time.sleep(0.7)
        host.bridge.call('virtual_pad', action='axis', slot=0, axis=1, value=0)
        end = _owned(host)
        assert math.dist(start[actual]['pos'], end[actual]['pos']) > 0.8, 'pad no longer controls P2 after campaign transition'
        assert math.dist(start[0]['pos'], end[0]['pos']) < 0.7, 'P2 pad also moved the story player'
        metrics['offline_progression'] = {'identity': identity, 'controls': controls, 'pads': pads,
                                          'status': _status(host), 'screenshot': _shot(ctx, host, 'campaign_level_two_local_party')}

        # Rewind the fixture to the first level's authored Civ1 scene. The two
        # preceding levels were advanced using the actual campaign levelwin.
        _code(host, 'LoadLevelID("' + FIRST_ID + '");')
        _ready(host, FIRST)
        _skip(host)
        assert _signature(host) == identity
        # Civ1 normally follows the nearby bandits' defeat. Keep their combat
        # AI quiet so a post-dialogue attack cannot masquerade as bad placement
        # or a health-restoration failure; their dialogue animation still runs.
        _code(host, 'for(int k=0;k<GetNumCharacters();++k) { MovementObject@ c=ReadCharacter(k); '
              'if(!c.controlled) c.ReceiveScriptMessage("set_combat_allowed false"); }')
        extra_id = next(p['avatar_id'] for p in _rows(host) if p['local_index'] == 1)
        _code(host, f'ReadCharacterID({extra_id}).visible=false; ReadCharacterID({extra_id}).Execute('
              '"blood_health=0.6f;temp_health=0.6f;permanent_health=0.6f;");')
        saved = next(c for c in host.bridge.state() if c['id'] == extra_id)
        _code(host, 'level.SendMessage("start_dialogue \\"Civ1\\"");')
        _wait(host, lambda: _status(host)['coop_cutscene'], 'authored Civ1 begins')
        # Keep this authored line on screen while a fresh client's assets load.
        # Camera focus, actor choreography and text came from the real script;
        # the world remains running, so this does not fake a paused freeze test.
        _code(host, 'dialogue.dialogue_text_disp_chars=float(dialogue.dialogue_text.length()); '
              'dialogue.waiting_for_dialogue=false;dialogue.is_waiting_time=true;dialogue.wait_time=90.0f;')
        chars = _assert_cutscene(host)
        main = chars[_status(host)['campaign_main_avatar']]
        assert math.dist(main['pos'], [34.6516, 13.3844, -8.43466]) < 1.0, 'main story choreography was overwritten'
        assert _probe(host, 'LobbyOpenToNetwork()') == 'true'
        assert not _status(host)['online']['local_only']
        guest = ctx.instance('campaign_local_guest', width=1280, height=720,
                             keys={'mp_port': '25845', 'playername': 'Campaign remote',
                                   'gamepad_1_look_sensitivity': '1.85', 'gamepad_1[jump]': 'y'})
        guest.bridge.call('connect', ip='127.0.0.1:25844')
        _ready(guest, FIRST)
        _wait(guest, lambda: _status(guest)['coop_cutscene'], 'late guest catches the actual dialogue')
        _assert_cutscene(guest, require_script=False)  # the early host hint already protects the joiner
        _wait(guest, lambda: _status(guest)['dialogue_camera_control'], 'authored dialogue text/camera synchronizes')
        assert _status(host)['coop_cutscene'], 'fixture dialogue ended before late admission'
        _join_pad(guest, 0, 2)
        for inst in (host, guest):
            _wait(inst, lambda i=inst: len(_human_chars(i)) == 4, 'two local players on each client')
            _wait(inst, lambda i=inst: _status(i)['coop_cutscene'] and _status(i)['render_views'] == 1,
                  'four-human cinematic remains full window')
            _assert_cutscene(inst)
        before = {i.name: _human_chars(i) for i in (host, guest)}
        for inst in (host, guest):
            for actual_input in _owned(inst):
                inst.bridge.call('input', controller=actual_input, hold={'up': 1}, steps=120)
        time.sleep(1)
        for inst in (host, guest):
            for actual_input in _owned(inst): inst.bridge.call('input', controller=actual_input, release_all=True)
            after = _assert_cutscene(inst)
            for actor in after.values():
                if actor['id'] != _status(inst)['campaign_main_avatar']:
                    assert math.dist(actor['pos'], before[inst.name][actor['id']]['pos']) < 0.2, 'hidden extra moved during dialogue'
            metrics[inst.name + '_cutscene'] = {'status': _status(inst), 'characters': after,
                                               'screenshot': _shot(ctx, inst, inst.name + '_full_window_dialogue')}
        _code(host, 'dialogue.is_waiting_time=false;dialogue.wait_time=0;dialogue.waiting_for_dialogue=false;')
        _skip(host)
        for inst in (host, guest):
            _wait(inst, lambda i=inst: not _status(i)['coop_cutscene'] and _status(i)['render_views'] == 2,
                  'dialogue exits to both local views')
        time.sleep(0.6)
        restored = _human_chars(host)[extra_id]
        assert not restored['visible'], 'dialogue exit discarded prior hidden state'
        for health in ('temp_health', 'blood_health', 'permanent_health'):
            assert restored[health] <= 0.65, ('dialogue exit healed extra player', health, saved, restored)
        assert restored['dialogue_control'] == saved['dialogue_control'] and not restored['coop_cutscene_hold']
        main_pos = _human_chars(host)[_status(host)['campaign_main_avatar']]['pos']
        for inst in (host, guest):
            for actor in _human_chars(inst).values():
                if actor['id'] == _status(inst)['campaign_main_avatar']: continue
                assert not actor['coop_cutscene_hold'], 'extra stays frozen after dialogue'
                if actor['common_id'] != restored['common_id']: assert actor['visible'], 'new player stays hidden after dialogue'
                if not actor['remote']:
                    assert actor['knocked_out'] == 0 and actor['on_ground'], 'extra does not stand safely after dialogue'
                    assert math.dist(actor['pos'], main_pos) < 6, 'extra restored far from story player'
        _code(host, f'ReadCharacterID({extra_id}).visible=true;')
        before_identity = _signature(host)
        guest_controls, guest_pads = _controls(guest, 1), _pad_map(guest)
        _advance(host, [host, guest], 4)
        _skip(host)
        for inst in (host, guest):
            _wait(inst, lambda i=inst: not _status(i)['coop_cutscene'] and len(_owned(i)) == 2,
                  'two owned characters survive network campaign progression')
            assert _signature(inst) == before_identity, 'network levelwin changed party roster'
            assert _status(inst)['local_inputs'] == 2
            assert not inst.bridge.call('nan_scan')['characters']
            assert not script_errors(inst), script_errors(inst)
        assert _controls(host, 1) == controls and _pad_map(host) == pads
        assert _controls(guest, 1) == guest_controls and _pad_map(guest) == guest_pads
        metrics['network_progression'] = {'host': _status(host), 'guest': _status(guest),
                                          'restored_low_health_extra': restored}
        return {'passed': True, 'metrics': metrics, 'notes': []}
    finally:
        for inst in (host, guest):
            if inst is not None and inst.bridge:
                try: inst.bridge.call('virtual_pad', action='clear')
                except Exception: pass


def newcampaign_roles(ctx):
    """A lobby's P2 decision host/leader does not replace the authored P1."""
    host = ctx.instance('campaign_roles_host', width=1280, height=720, keys={'mp_port': '25844'})
    guest = ctx.instance('campaign_roles_guest', width=1280, height=720, keys={'mp_port': '25845'})
    _code(host, 'LobbyBeginHost();LobbyAddLocalPlayer();', 'menu')
    _code(guest, 'LobbyAddLocalPlayer();', 'menu')
    guest.bridge.call('connect', ip='127.0.0.1:25844')
    _wait(host, lambda: len(_rows(host)) == 4, 'four lobby players')
    rows = _rows(host)
    hp2 = next(p for p in rows if p['peer_id'] == 255 and p['local_index'] == 1)
    gp2 = next(p for p in rows if p['peer_id'] != 255 and p['local_index'] == 1)
    _code(host, f'LobbySetPlayerName({hp2["id"]},"Story host P2");LobbySetPlayerScreen({hp2["id"]},1);'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          f'LobbyPromoteHost({hp2["id"]});LobbyPromoteLeader({hp2["id"]});', 'menu')
    _code(guest, f'LobbySetPlayerName({gp2["id"]},"Story guest P2");LobbySetPlayerScreen({gp2["id"]},1);', 'menu')
    _wait(host, lambda: next(p for p in _rows(host) if p['id'] == gp2['id'])['name'] == 'Story guest P2', 'custom guest names')
    signature = _signature(host)
    _code(host, 'SetControllerPlayer(1);LobbyStart();', 'menu')
    for inst in (host, guest): _ready(inst, FIRST)
    for inst in (host, guest):
        _wait(inst, lambda i=inst: _status(i)['coop_cutscene'], 'authored intro on both peers')
        chars = _assert_cutscene(inst)
        main = chars[_status(inst)['campaign_main_avatar']]
        story = next(p for p in _rows(inst) if p['avatar_id'] == main['id'])
        assert story['id'] == 255 and not story['host'] and not story['leader'], 'host promotion changed story identity'
        _shot(ctx, inst, inst.name + '_promoted_host_intro')
    assert _probe(host, 'GetCurrLevelID()') == FIRST_ID, 'lobby map did not load by authored campaign ID'
    _skip(host)
    _advance(host, [host, guest], 4)
    _skip(host)
    for inst in (host, guest):
        assert _signature(inst) == signature, 'roles/screens/names changed on campaign progression'
        assert _status(inst)['local_players'] == 2 and len(_owned(inst)) == 2
        assert not inst.bridge.call('nan_scan')['characters']
        assert not script_errors(inst), script_errors(inst)
    return {'passed': True, 'metrics': {'signature': signature, 'host': _status(host), 'guest': _status(guest)}, 'notes': []}


def newcampaign_main_removal(ctx):
    """Remove original P1; actual input1 must inherit the authored hero.

    Also render-isolate the nearby authored bandit363 in Civ1: this distinguishes
    its visible body from the extra humans deliberately hidden by the cutscene.
    """
    inst = ctx.instance('campaign_main_removal', width=1280, height=720,
                        keys={'mp_port': '25844', 'gamepad_1_look_sensitivity': '3.75'})
    _code(inst, 'LobbyBeginHost();LobbyAddLocalPlayer();', 'menu')
    survivor = next(p for p in _rows(inst) if p['local_index'] == 1)
    _code(inst, f'LobbySetPlayerName({survivor["id"]},"Surviving P2");'
          f'LobbyPromoteHost({survivor["id"]});LobbyPromoteLeader({survivor["id"]});'
          f'SetControllerPlayer(1);LobbyRemovePlayer(255);'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();', 'menu')
    _ready(inst, FIRST)
    _wait(inst, lambda: _status(inst)['coop_cutscene'], 'removed-P1 story intro')
    rows = _rows(inst)
    assert len(rows) == 1 and rows[0]['id'] == survivor['id'] and rows[0]['id'] != 255, rows
    assert rows[0]['host'] and rows[0]['leader'] and rows[0]['local_index'] == 0
    main = _status(inst)['campaign_main_avatar']
    hero = _human_chars(inst)[main]
    assert hero['id'] == rows[0]['avatar_id'] and hero['controller_id'] == 1, hero
    assert _probe(inst, 'GetCampaignDialogueControllerID()') == '1'
    assert abs(float(_probe(inst, 'GetConfigValueFloat("gamepad_0_look_sensitivity")')) - 3.75) < 0.01
    before = {'status': _status(inst), 'hero': hero, 'dialogue_input': 1,
              'screenshot': _shot(ctx, inst, 'campaign_removed_p1_intro')}
    _skip(inst, controller=1)
    _wait(inst, lambda: not _status(inst)['coop_cutscene'], 'actual input1 skips the intro')

    # Match the earlier Civ1 fixture: defeated-bandit combat is quiet, but the
    # authored actor remains rendered until explicitly hidden for comparison.
    _code(inst, 'for(int k=0;k<GetNumCharacters();++k) { MovementObject@ c=ReadCharacter(k); '
          'if(!c.controlled)c.ReceiveScriptMessage("set_combat_allowed false"); } '
          'level.SendMessage("start_dialogue \\"Civ1\\"");')
    _wait(inst, lambda: _status(inst)['coop_cutscene'], 'Civ1 render identification scene')
    _code(inst, 'dialogue.dialogue_text_disp_chars=float(dialogue.dialogue_text.length());'
          'dialogue.waiting_for_dialogue=false;dialogue.is_waiting_time=true;dialogue.wait_time=60.0f;')
    actors = inst.bridge.state()
    bandit = next(c for c in actors if c['id'] == 363)
    assert not bandit['is_player'] and bandit['id'] not in {p['avatar_id'] for p in _rows(inst)}, bandit
    before_image = _shot(ctx, inst, 'civ1_authored_npc363_visible')
    _code(inst, 'ReadCharacterID(363).visible=false;')
    time.sleep(0.15)
    after_image = _shot(ctx, inst, 'civ1_authored_npc363_hidden')
    after = next(c for c in inst.bridge.state() if c['id'] == 363)
    assert not after['visible']
    _code(inst, 'ReadCharacterID(363).visible=true;dialogue.is_waiting_time=false;'
          'dialogue.wait_time=0;dialogue.waiting_for_dialogue=false;')
    _skip(inst, controller=1)
    assert not script_errors(inst), script_errors(inst)
    return {'passed': True, 'metrics': {'main_replacement': before, 'all_civ1_actors': actors,
            'authored_npc363': bandit, 'visible_render': before_image, 'hidden_render': after_image,
            'final_status': _status(inst)}, 'notes': []}


def newcampaign_cinematic_retry(ctx):
    """A real active-level Retry clears old holds before actor IDs are reused."""
    host = ctx.instance('campaign_retry_host', width=1280, height=720,
                        keys={'mp_port': '25844', 'playername': 'Retry host',
                              'gamepad_1_look_sensitivity': '2.9', 'gamepad_1[jump]': 'b'})
    guest = ctx.instance('campaign_retry_guest', width=1280, height=720,
                         keys={'mp_port': '25845', 'playername': 'Retry guest',
                               'gamepad_1_look_sensitivity': '3.1', 'gamepad_1[jump]': 'y'})
    _code(host, 'LobbyBeginHost();LobbyAddLocalPlayer();', 'menu')
    _code(guest, 'LobbyAddLocalPlayer();', 'menu')
    guest.bridge.call('connect', ip='127.0.0.1:25844')
    _wait(host, lambda: len(_rows(host)) == 4, 'four retry-test humans')
    hp2 = next(p for p in _rows(host) if p['peer_id'] == 255 and p['local_index'] == 1)
    gp2 = next(p for p in _rows(guest) if p['peer_id'] != 255 and p['local_index'] == 1)
    _code(host, f'LobbySetPlayerName({hp2["id"]},"Retry host P2");LobbySetPlayerScreen({hp2["id"]},1);'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);', 'menu')
    _code(guest, f'LobbySetPlayerName({gp2["id"]},"Retry guest P2");LobbySetPlayerScreen({gp2["id"]},1);', 'menu')
    _wait(host, lambda: next(p for p in _rows(host) if p['id'] == gp2['id'])['name'] == 'Retry guest P2', 'retry names replicated')
    _code(host, 'LobbyStart();', 'menu')
    for inst in (host, guest): _ready(inst, FIRST)
    _skip(host)
    for inst in (host, guest):
        _wait(inst, lambda i=inst: not _status(i)['coop_cutscene'], 'initial intro ends')

    def routing(inst):
        chars = _human_chars(inst)
        result = {}
        for row in _rows(inst):
            actor = chars.get(row['avatar_id'])
            if actor is not None and not actor['remote']:
                assert actor['camera_id'] == row['screen'] - 1, ('screen does not own its assigned camera', row, actor)
                result[row['id']] = {'input': actor['controller_id'], 'camera': actor['camera_id']}
        assert len(result) == 2
        return result

    identity = _signature(host)
    profiles = {i.name: _controls(i, 1) for i in (host, guest)}
    routes = {i.name: routing(i) for i in (host, guest)}
    # Save distinctive state in the old scene. The retry's freshly created
    # actors should start healthy and visible, not restore these old flags.
    for inst in (host, guest):
        own_extra = next(c for c in _owned(inst).values() if next(
            p for p in _rows(inst) if p['avatar_id'] == c['id'])['local_index'] == 1)
        _code(inst, f'ReadCharacterID({own_extra["id"]}).visible=false;'
              f'ReadCharacterID({own_extra["id"]}).Execute("blood_health=0.6f;temp_health=0.6f;permanent_health=0.6f;");'
              'display_text="old-cinematic-before-retry";')
    _code(host, 'for(int k=0;k<GetNumCharacters();++k) { MovementObject@ c=ReadCharacter(k);'
          'if(!c.controlled)c.ReceiveScriptMessage("set_combat_allowed false"); }'
          'level.SendMessage("start_dialogue \\"Civ1\\"");')
    for inst in (host, guest):
        _wait(inst, lambda i=inst: _status(i)['dialogue_camera_control'] and _status(i)['coop_cutscene'], 'Civ1 active before retry')
        _assert_cutscene(inst)
    old = {i.name: {'status': _status(i), 'characters': _human_chars(i),
                    'screenshot': _shot(ctx, i, i.name + '_before_retry')} for i in (host, guest)}
    assert any(c.get('coop_cutscene_hold') for c in old[host.name]['characters'].values()), 'no active old hold to clear'

    # The pause menu's actual retry function resolves the authored campaign ID.
    # A same-path ready check alone could accidentally observe the old world;
    # the initialized AS marker below proves each level context was replaced.
    _code(host, 'OnlineRetry();')
    for inst in (host, guest):
        def reloaded(i=inst):
            status = _status(i)
            if not status['level_loaded'] or FIRST not in status['level']: return False
            if status['waiting_for_input'] and status['online']['hosting']:
                i.bridge.call('start_session')
                return False
            return not status['waiting_for_input'] and _probe(i, 'display_text') != 'old-cinematic-before-retry'
        _wait(inst, reloaded, 'fresh campaign script context after Retry', 240)
        _wait(inst, lambda i=inst: len(_human_chars(i)) == 4 and _status(i)['coop_cutscene'], 'fresh four-human opening cinematic')
        _assert_cutscene(inst)
        for actor in _owned(inst).values():
            if actor['id'] != _status(inst)['campaign_main_avatar']:
                for key in ('blood_health', 'permanent_health', 'temp_health'):
                    assert actor[key] > 0.95, ('old hold health contaminated fresh actor', inst.name, actor)
        _shot(ctx, inst, inst.name + '_fresh_intro')
    _skip(host)
    for inst in (host, guest):
        _wait(inst, lambda i=inst: not _status(i)['coop_cutscene'] and _status(i)['render_views'] == 2, 'retry restores both local views')
    time.sleep(0.5)
    fresh = {}
    for inst in (host, guest):
        assert _signature(inst) == identity, 'Retry changed party names/screens/roles'
        assert _controls(inst, 1) == profiles[inst.name] and routing(inst) == routes[inst.name], 'Retry changed controller profile or viewport ownership'
        for actor in _owned(inst).values():
            assert actor['visible'] and not actor['coop_cutscene_hold'], ('stale old hold/visibility survived Retry', actor)
        assert not inst.bridge.call('nan_scan')['characters']
        assert not script_errors(inst), script_errors(inst)
        fresh[inst.name] = {'status': _status(inst), 'characters': _human_chars(inst),
                            'screenshot': _shot(ctx, inst, inst.name + '_after_retry')}
    return {'passed': True, 'metrics': {'old_scene': old, 'fresh_scene': fresh,
            'identity': identity, 'profiles': profiles, 'routing': routes}, 'notes': []}
