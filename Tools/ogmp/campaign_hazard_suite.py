"""Authored climbing death floors follow each human, including local extras.

Sky Ark: one human reaches checkpoint6 while two others remain below the
newly armed floor7; both laggards stay safe, the advanced human's floor remains
lethal, and revival/late local join adopt the lower anchor's course. Then the
same party changes chapter and tests Waterfall Arena's unconditional floor.
"""
import math
import os
import time
import traceback

from .scenarios import script_errors

CAMPAIGN = 'com-wolfire-overgrowth-campaign'
SKY = 'og_story/27_Sky_Ark.xml'
WATERFALL = 'og_story/22_Waterfall_Arena.xml'


def campaign_hazards(ctx):
    host = ctx.instance('hazard_host', width=1280, height=720,
                        keys={'mp_port': '25844', 'playername': 'Course host', 'master_volume': '0'})
    guest = ctx.instance('hazard_guest', width=1280, height=720,
                         keys={'mp_port': '25845', 'playername': 'Course guest', 'master_volume': '0'})
    metrics = {}

    def code(inst, text, target='level'):
        return inst.bridge.call('exec', target=target, code=text, timeout=240)

    def status(inst):
        return inst.bridge.call('status', timeout=240)

    def rows(inst):
        return status(inst)['online']['players']

    def wait(inst, predicate, label, seconds=35):
        return inst.bridge.wait(predicate, seconds, step=0.1, what=label)

    def probe(inst, expression):
        inst.bridge.call('level_messages', prefix='hazard_probe')
        code(inst, 'level.SendMessage("hazard_probe " + (' + expression + '));')
        messages = inst.bridge.call('level_messages', prefix='hazard_probe')['messages']
        assert messages, 'hazard probe did not execute: ' + expression
        return messages[-1].split(' ', 1)[1]

    def course(inst, level_hotspot, avatar):
        inst.bridge.call('level_messages', prefix='hazard_probe')
        code(inst, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
             f'if(h.GetID()=={level_hotspot}) level.SendMessage("hazard_probe "+'
             f'h.QueryIntFunction("int CoopCourseProgressForCharacter(int)",{avatar}));' + '}')
        result = inst.bridge.call('level_messages', prefix='hazard_probe')['messages']
        assert result, 'authored course hotspot not found'
        return int(result[-1].split()[-1])

    def progress(inst, level_hotspot):
        return int(probe(inst, f'"" + cast<Hotspot@>(ReadObjectFromID({level_hotspot})).GetIntVar("progress")'))

    def vec(inst, expression):
        value = probe(inst, '"" + (' + expression + ').x + " " + (' + expression + ').y + " " + (' + expression + ').z')
        return [float(v) for v in value.split()]

    def freeze(inst):
        code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(c.controlled&&!c.network_proxy)c.static_char=true;'
             'else if(!c.controlled&&!c.network_proxy)c.ReceiveScriptMessage("set_combat_allowed false");}')

    def teleport(inst, avatar, position, frozen=True):
        position = ','.join(str(v) for v in position)
        code(inst, f'MovementObject@ c=ReadCharacterID({avatar});c.static_char={str(frozen).lower()};'
             f'c.position=vec3({position});c.velocity=vec3(0);c.Execute("SetOnGround(false);FixDiscontinuity();");')

    def flags(inst, avatar):
        return probe(inst, f'""+ReadCharacterID({avatar}).GetBoolVar("fall_death")+" "+'
                     f'ReadCharacterID({avatar}).GetIntVar("knocked_out")+" "+'
                     f'ReadCharacterID({avatar}).GetIntVar("zone_killed")').split()

    def ready(inst, path):
        def loaded():
            value = status(inst)
            if not value['level_loaded'] or path not in value['level']:
                return False
            if value['waiting_for_input'] and value['online']['hosting']:
                inst.bridge.call('start_session')
                return False
            return not value['waiting_for_input']
        wait(inst, loaded, 'authored campaign chapter started', 240)

    def skip():
        for _ in range(15):
            if not status(host)['dialogue_camera_control'] and not status(guest)['coop_cutscene']:
                return
            host.bridge.call('input', controller=0, hold={'skip_dialogue': 1}, steps=10)
            time.sleep(0.45)
        raise AssertionError('authored opening dialogue did not finish on normal skip input')

    def visit(inst, avatar, checkpoint, expected, level_hotspot=737):
        center = vec(inst, f'ReadObjectFromID({checkpoint}).GetTranslation()')
        teleport(inst, avatar, [center[0], center[1] + 8, center[2]])
        time.sleep(0.15)
        teleport(inst, avatar, center)
        wait(inst, lambda: course(inst, level_hotspot, avatar) >= expected,
             'actual timed checkpoint contact advances only its human', 12)

    def geometry(inst):
        return {str(i): {'position': vec(inst, f'ReadObjectFromID({i}).GetTranslation()'),
                         'scale': vec(inst, f'ReadObjectFromID({i}).GetScale()')}
                for i in (3655, 3670, 3677, 3664)}

    def shot(inst, name):
        path = os.path.abspath(os.path.join(ctx.out_dir, name + '.png'))
        inst.bridge.call('screenshot', path=path)
        return path

    try:
        code(host, 'LobbyBeginHost();LobbyAddLocalPlayer();this_ui.SendCallback("multiplayer_lobby.as");', 'menu')
        guest.bridge.call('connect', ip='127.0.0.1:25844')
        for inst in (host, guest):
            wait(inst, lambda i=inst: len(rows(i)) == 3, 'two host locals and a remote admitted')
        code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{SKY}",false);LobbyStart();', 'menu')
        for inst in (host, guest): ready(inst, SKY)
        skip()
        for inst in (host, guest):
            wait(inst, lambda i=inst: len([p for p in rows(i) if p['avatar_id'] >= 0]) == 3, 'three human bodies')
            freeze(inst)
        hp = next(p for p in rows(host) if p['peer_id'] == 255 and p['local_index'] == 0)
        h2 = next(p for p in rows(host) if p['peer_id'] == 255 and p['local_index'] == 1)
        gp = next(p for p in rows(guest) if p['name'] == 'Course guest')
        initial_geometry = geometry(host)
        assert initial_geometry == geometry(guest), 'peers loaded different authored death-floor geometry'
        for inst, avatar in ((host, hp['avatar_id']), (host, h2['avatar_id']), (guest, gp['avatar_id'])):
            wait(inst, lambda i=inst, a=avatar: course(i, 737, a) == 0, 'initial personal course baseline')
        visit(host, hp['avatar_id'], 3675, 7)
        for inst in (host, guest): wait(inst, lambda i=inst: progress(i, 737) == 7, 'shared story reaches checkpoint7')
        assert geometry(host) == initial_geometry == geometry(guest), 'arming a course floor changed its transform'
        npc = next(c for c in host.bridge.state() if not c['controlled'] and not c['is_player'])
        assert course(host, 737, npc['id']) == 7, 'personal-human policy disabled an NPC hazard'
        metrics['npc_original_hazard_stage'] = {'avatar': npc['id'], 'stage': 7}

        air = [-200, 105, 180]
        p = 'vec3(' + ','.join(map(str, air)) + ')'
        assert probe(guest, f'"" + (distance(col.GetRayCollision({p},{p}-vec3(0,3.5,0)),{p}-vec3(0,3.5,0))<0.01f)') == 'true', \
            'fixture needs air farther than the former 3.5m ground exception'
        teleport(guest, gp['avatar_id'], air)
        teleport(host, h2['avatar_id'], [air[0] + 3, air[1], air[2]])
        time.sleep(0.6)
        assert flags(guest, gp['avatar_id'])[:2] == ['false', '0'], 'remote laggard hit another human\'s advancing floor'
        assert flags(host, h2['avatar_id'])[:2] == ['false', '0'], 'second local inherited the main human\'s floor'
        assert course(guest, 737, gp['avatar_id']) == course(host, 737, h2['avatar_id']) == 0
        metrics['separated'] = {'host_rows': rows(host), 'guest_rows': rows(guest),
                                'host_flags': flags(host, hp['avatar_id']), 'local2_flags': flags(host, h2['avatar_id']),
                                'guest_flags': flags(guest, gp['avatar_id']), 'geometry': initial_geometry,
                                'screenshot': shot(guest, 'guest_below_another_human_floor')}
        print('campaign_hazards separated host/local2/guest gates PASS', flush=True)

        # Put a living lower-stage anchor on real ground, then make the advanced
        # human fall back through their own armed floor onto an earlier ledge.
        visit(guest, gp['avatar_id'], 3665, 1)
        ledge = vec(host, 'ReadObjectFromID(768).GetTranslation()')
        teleport(host, hp['avatar_id'], [ledge[0], ledge[1] + 10, ledge[2]])
        time.sleep(0.15)
        teleport(host, hp['avatar_id'], ledge)
        wait(host, lambda: flags(host, hp['avatar_id'])[0] == 'true', 'advanced human own floor remains armed')
        code(host, 'this_mo.static_char=false;', hp['avatar_id'])
        wait(host, lambda: flags(host, hp['avatar_id'])[1] != '0', 'own armed floor causes a real knockout', 12)
        metrics['armed_floor_kills'] = flags(host, hp['avatar_id'])
        wait(host, lambda: flags(host, hp['avatar_id'])[:2] == ['false', '0'], 'zone revival finds lower teammate ground', 18)
        wait(host, lambda: course(host, 737, hp['avatar_id']) == 1, 'revive adopts lower anchor course')
        guest_position = next(c for c in guest.bridge.state() if c['id'] == gp['avatar_id'])['pos']
        revived = next(c for c in host.bridge.state() if c['id'] == hp['avatar_id'])['pos']
        assert math.dist(guest_position, revived) < 8, ('revived away from lower-stage anchor', guest_position, revived)
        freeze(host)
        code(guest, 'DebugAddLocalPlayer();')
        for inst in (host, guest):
            wait(inst, lambda i=inst: len(rows(i)) == 4 and all(p['avatar_id'] >= 0 for p in rows(i)), 'late second guest local joins')
        late = next(p for p in rows(guest) if p['peer_id'] == gp['peer_id'] and p['local_index'] == 1)
        wait(guest, lambda: course(guest, 737, late['avatar_id']) == 1, 'late local uses actual lower anchor, not shared progress7')
        freeze(guest)
        assert flags(guest, late['avatar_id'])[:2] == ['false', '0']
        metrics['revive_and_latejoin'] = {'revived': revived, 'anchor': guest_position, 'late': late,
                                         'late_stage': course(guest, 737, late['avatar_id']), 'rows': rows(host)}
        print('campaign_hazards real knockout/revive and late local anchor PASS', flush=True)

        identity = {p['id']: (p['name'], p['local_index'], p['screen']) for p in rows(host)}
        host.bridge.call('load_level', path='load_campaign_level waterfall_arena')
        for inst in (host, guest): ready(inst, WATERFALL)
        skip()
        for inst in (host, guest): freeze(inst)
        assert {p['id']: (p['name'], p['local_index'], p['screen']) for p in rows(host)} == identity
        late = next(p for p in rows(guest) if p['id'] == late['id'])
        wait(guest, lambda: course(guest, 314, late['avatar_id']) == 0, 'new chapter clears previous course')
        bottom = vec(guest, 'ReadObjectFromID(355).GetTranslation()')
        teleport(guest, late['avatar_id'], [bottom[0], bottom[1] + 25, bottom[2]])
        time.sleep(0.15)
        teleport(guest, late['avatar_id'], bottom)
        wait(guest, lambda: flags(guest, late['avatar_id'])[0] == 'true', 'unconditional authored floor still arms at personal stage0')
        metrics['second_chapter_unconditional'] = {'rows': rows(guest), 'flags': flags(guest, late['avatar_id']),
                                                   'stage': course(guest, 314, late['avatar_id'])}
        for inst in (host, guest):
            assert not script_errors(inst), (inst.name, script_errors(inst))
            assert status(inst)['gl_problems'] == status(inst).get('vk_problems', 0) == 0
        return {'passed': True, 'metrics': metrics, 'notes': []}
    except Exception:
        metrics['failure'] = traceback.format_exc()
        for inst in (host, guest):
            try: metrics[inst.name + '_failed_status'] = status(inst)
            except Exception: pass
        return {'passed': False, 'metrics': metrics, 'notes': [metrics['failure']]}
    finally:
        host.stop()
        guest.stop()
