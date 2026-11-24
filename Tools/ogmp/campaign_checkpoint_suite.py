"""Human-only campaign defeat, spectator input and allied bot regrouping.

Uses White Flags' original starting point and first authored checkpoint. Only
NPC movement is held; human character updates and normal attack inputs run.
"""

import math
import time
import traceback
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _skip
from .scenarios import script_errors


def campaign_bot_checkpoints(ctx):
    host = ctx.instance("bot_checkpoint_host", width=640, height=360,
                        keys={**RENDER_KEYS, "mp_port": "28074", "playername": "Checkpoint host",
                              "tutorials": "false", "campaign_mp_scaling": "false", "player_invincible": "true",
                              "gamepad_0_look_sensitivity": "2.75", "gamepad_0_invert_x_look": "true",
                              "gamepad_0[jump]": "b"})
    guest = None
    metrics = {}
    ctx.campaign_checkpoint_metrics = metrics

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text)

    def status(inst=host):
        return inst.bridge.status()

    def wait(predicate, label, seconds=25):
        return host.bridge.wait(predicate, seconds, step=0.08, what=label)

    def rows(inst=host):
        return status(inst)["online"]["players"]

    def world(inst=host):
        return {c["id"]: c for c in inst.bridge.state()}

    def probe(inst, expression):
        # Reading these diagnostics must not drain the reset-message trace.
        code(inst, 'SetConfigValueString("test_checkpoint_probe",""+(' + expression + '));SaveConfig();')
        config = dict(line.split(": ", 1) for line in
                      (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        return config["test_checkpoint_probe"]

    def progress(inst=host):
        code(inst, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
             'if(h.GetID()==14)SetConfigValueString("test_checkpoint_probe",""+h.GetIntVar("progress"));}SaveConfig();')
        config = dict(line.split(": ", 1) for line in
                      (Path(inst.writedir) / "Data/config.txt").read_text().splitlines() if ": " in line)
        return int(config["test_checkpoint_probe"])

    def identity(inst=host):
        return {p["id"]: {k: p[k] for k in ("name", "team", "screen", "host", "leader", "peer_id", "local_index", "bot")}
                for p in rows(inst)}

    def controls(inst):
        return probe(inst, 'GetBindingValue("gamepad_0","jump")+"|"+'
                     'GetConfigValueFloat("gamepad_0_look_sensitivity")+"|"+'
                     'GetConfigValueBool("gamepad_0_invert_x_look")')

    def freeze_npcs():
        # Humans remain active so the legacy death-input path is exercised.
        code(host, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(!c.controlled){c.ReceiveScriptMessage("set_combat_allowed false");c.static_char=true;}}')

    def party_awake():
        actors = world()
        return all(p["avatar_id"] in actors and actors[p["avatar_id"]].get("knocked_out") == 0 for p in rows())

    def kill(inst, avatar, zone=False):
        hazard = 'ReceiveMessage("fall_death");zone_killed=1;' if zone else ''
        code(inst, 'this_mo.static_char=false;' + hazard + 'SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);', avatar)

    def attack(inst, controller=0):
        inst.bridge.call("input", controller=controller, hold={"attack": 1}, steps=8)
        time.sleep(0.1)
        inst.bridge.call("input", release_all=True)
        time.sleep(0.1)

    def key(inst, name):
        for down in (True, False):
            inst.bridge.call("key", key=name, down=down)
            time.sleep(0.1)

    def pad(button):
        for down in (True, False):
            host.bridge.call("virtual_pad", action="button", slot=0, button=button, down=down)
            time.sleep(0.1)

    def cycle_without_retry(inst, avatar, controller=False):
        wait(lambda: probe(inst, 'Online_Spectating()') != "", "downed human starts watching an awake teammate")
        before = probe(inst, 'Online_Spectating()')
        pad(10) if controller else key(inst, "E")
        wait(lambda: probe(inst, 'Online_Spectating()') not in ("", before), "next input cycles the spectator target", 1.5)
        after = probe(inst, 'Online_Spectating()')
        pad(9) if controller else key(inst, "Q")
        wait(lambda: probe(inst, 'Online_Spectating()') == before, "previous input returns to the original target", 1.5)
        assert world(inst)[avatar]["knocked_out"] != 0, "spectator navigation restarted the level"
        return [before, after, before]

    def regrouped(expected_progress, label):
        _skip(host)
        wait(lambda: not status()["coop_cutscene"] and not status()["dialogue_camera_control"], label + " dialogue ends")
        freeze_npcs()

        def nearby():
            actors = world()
            main = actors.get(status()["campaign_main_avatar"])
            if not main or main["knocked_out"] != 0:
                return False
            for player in rows():
                actor = actors.get(player["avatar_id"])
                if not actor or actor["knocked_out"] != 0 or not actor.get("on_ground"):
                    return False
                if math.dist(actor["pos"], main["pos"]) > 12 or abs(actor["pos"][1] - main["pos"][1]) > 3:
                    return False
            return True

        wait(nearby, label + " all surviving bots and humans stand beside the checkpoint party", 12)
        assert progress() == expected_progress, (label, "checkpoint progress changed", progress())
        actors = world()
        party = [{"id": p["id"], "name": p["name"], "bot": p["bot"],
                  "pos": actors[p["avatar_id"]]["pos"], "on_ground": actors[p["avatar_id"]]["on_ground"]}
                 for p in rows()]
        scan = host.bridge.call("nan_scan")
        assert not any(scan.get(key) for key in ("characters", "objects", "cameras", "invalid_decal_geometry")), scan
        return party

    def single_human_reset(expected_progress, label, cycle=True, zone=False, manual=False):
        hero = status()["campaign_main_avatar"]
        assert len([p for p in rows() if not p["bot"]]) == 1
        assert len([p for p in rows() if p["bot"]]) == 7
        assert party_awake(), (label, "party is already down")
        before_roster, before_controls = identity(), controls(host)
        host.bridge.call("level_messages", prefix="reset")
        started = time.monotonic()
        kill(host, hero, zone=zone)
        wait(lambda: world()[hero]["knocked_out"] != 0, label + " human is down")
        watched = cycle_without_retry(host, hero) if cycle else []
        if manual:
            attack(host)
        wait(lambda: world()[hero]["knocked_out"] == 0, label + " all-human defeat automatically restarts", 12)
        elapsed = time.monotonic() - started
        if manual:
            assert elapsed < 4.5, (label, "attack did not request an early checkpoint retry", elapsed)
        else:
            assert 4.5 <= elapsed <= 11, (label, "automatic reset did not use the five-second delay", elapsed)
        resets = host.bridge.call("level_messages", prefix="reset")["messages"]
        assert "reset" in resets, (label, "no actual level reset was observed", resets)
        party = regrouped(expected_progress, label)
        assert identity() == before_roster, (label, "reset changed roster names, teams or screens")
        assert controls(host) == before_controls, (label, "reset changed personal bindings")
        metrics[label] = {"reset_seconds": round(elapsed, 3), "spectator_cycle": watched, "party": party}

    try:
        code(host, 'LobbyBeginHost();for(int i=0;i<7;++i)LobbyAddBot();'
             'array<LobbyPlayer> party=LobbyPlayers();for(uint i=0;i<party.length();++i){'
             'if(party[i].bot)LobbySetPlayerName(party[i].id,"Checkpoint bot "+party[i].id);}'
             f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();', "menu")
        _ready(host, FIRST)
        freeze_npcs()
        _skip(host)
        wait(lambda: not status()["coop_cutscene"], "opening cutscene releases the party")
        freeze_npcs()
        single_human_reset(0, "starting_checkpoint", cycle=False)

        # Enter the first real objective checkpoint, without replacing goals.
        code(host, 'MovementObject@ hero=ReadCharacterID(0);hero.static_char=false;'
             'hero.position=ReadObjectFromID(478).GetTranslation();hero.velocity=vec3(0);'
             'hero.Execute("FixDiscontinuity();SetOnGround(true);");'
             'ReadObjectFromID(14).ReceiveScriptMessage("player_entered_checkpoint 0 0 477");')
        wait(lambda: progress() == 1, "authored checkpoint0 advances the story to progress1")
        _skip(host)
        freeze_npcs()
        single_human_reset(1, "progressed_checkpoint", manual=True)
        single_human_reset(1, "progressed_hazard_checkpoint", cycle=False, zone=True)

        # A real joined pad supplies shoulder input and a non-host local retry.
        host.bridge.call("virtual_pad", action="attach", slot=0)
        time.sleep(0.15)
        pad(0)
        wait(lambda: len(rows()) == 9, "virtual controller joins as local P2")
        local_pad = next(p for p in rows() if not p["bot"] and p["local_index"] == 1)
        pad_seat = local_pad["id"]
        wait(lambda: any(p["id"] == pad_seat and p["avatar_id"] in world() and
                         world()[p["avatar_id"]]["knocked_out"] == 0 for p in rows()),
             "local P2 has a live body")
        local_pad = next(p for p in rows() if p["id"] == pad_seat)
        assert not local_pad["host"], "controller fixture must exercise a non-host local player"
        freeze_npcs()
        original_local = identity()
        hero = status()["campaign_main_avatar"]
        pad_avatar = local_pad["avatar_id"]
        pad_input = world()[pad_avatar]["controller_id"]
        kill(host, hero)
        kill(host, pad_avatar)
        watched = cycle_without_retry(host, hero, controller=True)
        started = time.monotonic()
        attack(host, controller=pad_input)
        wait(lambda: world()[hero]["knocked_out"] == 0 and world()[pad_avatar]["knocked_out"] == 0,
             "non-host local P2 requests an early checkpoint retry", 3)
        elapsed = time.monotonic() - started
        assert elapsed < 4.5
        party = regrouped(1, "local_controller_checkpoint")
        assert identity() == original_local
        metrics["local_controller_checkpoint"] = {"reset_seconds": round(elapsed, 3), "spectator_cycle": watched,
                                                    "request_input": pad_input, "party": party}
        guest = ctx.instance("bot_checkpoint_guest", width=640, height=360,
                             keys={**RENDER_KEYS, "mp_port": "28075", "playername": "Checkpoint guest",
                                   "player_invincible": "true", "gamepad_0_look_sensitivity": "3.25",
                                   "gamepad_0_invert_x_look": "false", "gamepad_0[jump]": "y"})
        guest.bridge.call("connect", ip="127.0.0.1:28074")
        _ready(guest, FIRST)
        wait(lambda: len(rows()) == len(rows(guest)) == 10, "remote human joins the local controller party")
        wait(lambda: party_awake(), "late human and all bots are awake")
        freeze_npcs()
        original = identity()
        profiles = {host.name: controls(host), guest.name: controls(guest)}
        hero = status()["campaign_main_avatar"]
        remote = status(guest)["my_avatar"]
        host.bridge.call("level_messages", prefix="reset")
        kill(host, pad_avatar)
        kill(host, hero)
        started = time.monotonic()
        watched = cycle_without_retry(host, hero)
        watched_before_attack = probe(host, 'Online_Spectating()')
        time.sleep(1.15)
        attack(host)
        assert probe(host, 'Online_Spectating()') == watched_before_attack, "attack still changed the spectator target"
        assert world()[hero]["knocked_out"] != 0, "attack retried while another human was alive"
        while time.monotonic() - started < 6.0:
            assert world()[hero]["knocked_out"] != 0, "living remote human did not prevent a party restart"
            assert world(guest)[remote]["knocked_out"] == 0
            time.sleep(0.1)
        assert not host.bridge.call("level_messages", prefix="reset")["messages"], "spectator click reset while a human was still alive"
        assert progress() == progress(guest) == 1
        assert all(world()[p["avatar_id"]]["knocked_out"] == 0 for p in rows() if p["bot"])
        kill(guest, remote)
        wait(lambda: all(c["knocked_out"] != 0 for c in world().values() if c["controlled"]), "host observes both humans down")
        started = time.monotonic()
        wait(lambda: world()[hero]["knocked_out"] == 0 and world(guest)[remote]["knocked_out"] == 0,
             "all humans down restarts both peers despite living bots", 12)
        elapsed = time.monotonic() - started
        assert 4.3 <= elapsed <= 11, ("network checkpoint reset delay", elapsed)
        party = regrouped(1, "network_checkpoint")
        wait(lambda: progress(guest) == 1, "guest retains the authoritative checkpoint")
        assert identity() == original and identity(guest) == original
        assert {host.name: controls(host), guest.name: controls(guest)} == profiles
        metrics["network_checkpoint"] = {"reset_seconds": round(elapsed, 3), "spectator_cycle": watched, "party": party}

        # A non-host human can request an early retry once every human is down.
        kill(host, pad_avatar)
        kill(host, hero)
        kill(guest, remote)
        wait(lambda: all(c["knocked_out"] != 0 for c in world().values() if c["controlled"]),
             "host confirms all humans down before the remote retry request")
        started = time.monotonic()
        attack(guest)
        wait(lambda: world()[hero]["knocked_out"] == 0 and world(guest)[remote]["knocked_out"] == 0,
             "remote human's attack requests an early checkpoint retry", 3)
        elapsed = time.monotonic() - started
        assert elapsed < 4.5, ("remote retry fell back to the automatic timer", elapsed)
        party = regrouped(1, "remote_manual_checkpoint")
        assert identity() == identity(guest) == original
        metrics["remote_manual_checkpoint"] = {"reset_seconds": round(elapsed, 3), "party": party}

        # True solo retains the game's manual retry behavior.
        guest.bridge.call("disconnect")
        wait(lambda: len(rows()) == 9, "remote human leaves")
        code(host, 'LobbyEndGame();')
        wait(lambda: status()["online"]["lobby_waiting"] and not status()["level_loaded"], "host ends the game for lobby-only human removal")
        code(host, "".join(f'LobbyRemovePlayer({p["id"]});' for p in rows() if p["bot"] or p["id"] == pad_seat), "menu")
        host.bridge.call("virtual_pad", action="detach", slot=0)
        wait(lambda: len(rows()) == 1, "bot-free solo party")
        code(host, 'LobbyStart();', "menu")
        _ready(host, FIRST)
        _skip(host)
        freeze_npcs()
        hero = status()["campaign_main_avatar"]
        kill(host, hero)
        time.sleep(1.3)
        started = time.monotonic()
        attack(host)
        wait(lambda: world()[hero]["knocked_out"] == 0, "true solo attack still retries", 3)
        metrics["true_solo_manual_retry_seconds"] = round(time.monotonic() - started, 3)
        for inst in (host, guest):
            assert not script_errors(inst), (inst.name, script_errors(inst))
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        failure = traceback.format_exc()
        try:
            metrics["failure_host_status"] = status()
            metrics["failure_party"] = [c for c in world().values() if c["id"] in {p["avatar_id"] for p in rows()}]
            metrics["failure_progress"] = progress()
        except Exception:
            pass
        return {"passed": False, "metrics": metrics, "notes": [failure]}
    finally:
        for inst in (host, guest):
            if inst:
                inst.stop()
                (Path(ctx.out_dir) / (inst.name + ".log")).write_text(inst.log())
