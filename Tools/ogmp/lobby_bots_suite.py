"""Managed AI roster: UI, authority, escort, screens and campaign lifecycle.

The separate roster_bots_arena scenario covers Arena scoring and AI rounds.
"""

import math
import os
import time
import traceback

from .scenarios import LEVEL, script_errors, wait_loaded


def lobby_bots(ctx):
    metrics = {}
    all_instances = []

    def run(inst, code, player=None, target="menu"):
        args = {} if player is None else {"local_player": player}
        return inst.bridge.call("exec", target="player_menu" if player is not None else target,
                                code=code, **args)

    def state(inst):
        return inst.bridge.status()

    def rows(inst):
        return state(inst)["online"].get("players", [])

    def bots(inst):
        return [player for player in rows(inst) if player.get("bot")]

    def wait(inst, predicate, description, timeout=30):
        return inst.bridge.wait(predicate, timeout, what=description)

    def start_menu(inst, player=0):
        inst.bridge.call("input", controller=player, hold={"quit": 1}, steps=1)
        wait(inst, lambda: player in state(inst)["open_menu_players"], "personal menu")

    def action(inst, message, player=0):
        run(inst, f'imGUI.receiveMessage(IMMessage("{message}"));', player)
        time.sleep(0.15)

    def probe(inst, expression, player=0):
        inst.bridge.call("level_messages", prefix="bot_probe")
        run(inst, 'SetConfigValueString("test_bot_probe", "" + (' + expression + '));', player)
        run(inst, 'level.SendMessage("bot_probe " + GetConfigValueString("test_bot_probe"));', target="level")
        messages = inst.bridge.call("level_messages", prefix="bot_probe")["messages"]
        assert messages, "personal UI probe did not execute"
        return messages[-1].split(" ", 1)[1]

    def actor(inst, player):
        return next((entry for entry in inst.bridge.state() if entry["id"] == player["avatar_id"]), None)

    def screenshot(inst, name):
        path = os.path.abspath(os.path.join(ctx.out_dir, name + ".png"))
        inst.bridge.call("screenshot", path=path)
        wait(inst, lambda: os.path.isfile(path), "screenshot")

    def loaded(inst):
        wait(inst, lambda: state(inst)["level_loaded"] and not state(inst)["waiting_for_input"],
             "level and first frame", 180)

    def end_match(host, guest):
        run(host, "LobbyEndGame();", target="level")
        for inst in (host, guest):
            wait(inst, lambda i=inst: state(i)["online"]["lobby_waiting"] and not state(i)["level_loaded"],
                 "return to lobby", 60)

    try:
        solo = ctx.instance("lobby_bots_solo", level=LEVEL, width=1280, height=720,
                            keys={"master_volume": "0", "gl_debug": "1"})
        all_instances.append(solo)
        wait_loaded(solo)
        initial = state(solo)
        start_menu(solo)
        assert state(solo)["paused"], "ordinary single player must pause on Start"
        action(solo, "add_lobby_bot")
        wait(solo, lambda: len(bots(solo)) == 1, "Start menu adds managed bot")
        assert state(solo)["paused"], "one human with AI must retain single-player auto-pause"
        assert state(solo)["local_players"] == state(solo)["local_inputs"] == state(solo)["render_views"] == 1
        bot = bots(solo)[0]
        assert bot["screen"] == 0 and not bot["host"] and not bot["leader"]
        assert actor(solo, bot) is not None and not actor(solo, bot)["controlled"]
        action(solo, "Teams")
        assert probe(solo, "personal_teams") == "true"
        run(solo, f"OpenBotNameMenu({bot['id']});", 0)
        assert probe(solo, "personal_bot_name") == "true"
        run(solo, 'bot_name_editor.ClearText();bot_name_editor.UpdateText();'
            'string name="SOLO FRIEND";for(uint n=0;n<name.length();++n)'
            'imGUI.receiveMessage(IMMessage("bot_name_character",name.substr(n,1)));', 0)
        time.sleep(0.2)
        screenshot(solo, "bot_name_controller_keyboard")
        action(solo, "activate_bot_name")
        for digit in "09":
            solo.bridge.call("key", key="Keypad " + digit)
            time.sleep(.1)
        action(solo, "save_bot_name")
        wait(solo, lambda: bots(solo)[0]["name"] == "SOLO FRIEND09", "pause UI accepts keypad digits in a name")
        screenshot(solo, "bot_team_row")
        run(solo, f'LobbySetPlayerTeam({bot["id"]},1);MenuBack();', 0)
        action(solo, "Continue")
        wait(solo, lambda: not state(solo)["open_menu_players"] and not state(solo)["paused"], "single-player resumes")
        first = actor(solo, bots(solo)[0])["pos"]
        hero = next(player for player in rows(solo) if not player["bot"])
        hero_start = actor(solo, hero)["pos"]
        solo.bridge.call("input", controller=0, hold={"up": 1}, steps=200)
        time.sleep(2)
        solo.bridge.call("input", controller=0, release_all=True)
        wait(solo, lambda: math.dist(first, actor(solo, bots(solo)[0])["pos"]) > 1.0,
             "idle ally bot follows the moving human", 12)
        hero_end = actor(solo, hero)["pos"]
        bot_end = actor(solo, bots(solo)[0])["pos"]
        metrics["solo"] = {"bot_id": bot["id"], "human_moved_m": math.dist(hero_start, hero_end),
                           "bot_moved_m": math.dist(first, bot_end), "escort_distance_m": math.dist(hero_end, bot_end),
                           "local_inputs": state(solo)["local_inputs"], "render_views": state(solo)["render_views"],
                           "paused_with_bot": True, "initial_level": initial["level"]}
        assert metrics["solo"]["human_moved_m"] > 2
        solo.bridge.call("exec", target=hero["avatar_id"], code="SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);")
        wait(solo, lambda: actor(solo, hero)["knocked_out"] != 0, "solo human knocked out")
        assert actor(solo, bots(solo)[0])["knocked_out"] == 0, "reset fixture requires a living bot"
        run(solo, 'level.SendMessage("reset");', target="level")
        wait(solo, lambda: actor(solo, hero)["knocked_out"] == 0, "living bot does not block human reset", 10)
        metrics["solo"]["human_reset_with_living_bot"] = True
        solo.stop()
        print("lobby_bots: SP UI, pause and escort passed", flush=True)

        port = os.environ.get("OGMP_BOTS_PORT", "26964")
        keys = {"mp_port": port, "master_volume": "0", "gl_debug": "1", "oo_arena_random": "0"}
        host = ctx.instance("lobby_bots_host", width=1280, height=720, keys={**keys, "playername": "Host"})
        guest = ctx.instance("lobby_bots_guest", width=1280, height=720, keys={**keys, "playername": "Guest"})
        all_instances.extend([host, guest])
        host.bridge.call("menu", script="multiplayer_menu.as")
        run(host, 'if(LobbyBeginHost()){LobbyAddLocalPlayer();LobbyAddBot();LobbyAddBot();this_ui.SendCallback("multiplayer_lobby.as");}')
        guest.bridge.call("connect", ip="127.0.0.1")
        for inst in (host, guest):
            wait(inst, lambda i=inst: len(rows(i)) == 5 and len(bots(i)) == 2, "three humans and two bots")
        retained = {bot["id"] for bot in bots(host)}
        first_bot = bots(host)[0]["id"]
        run(guest, f'LobbyAddBot();LobbySetPlayerName({first_bot},"Stolen");LobbySetPlayerTeam({first_bot},3);LobbyRemovePlayer({first_bot});')
        time.sleep(0.3)
        assert len(bots(host)) == 2 and all(bot["name"] != "Stolen" for bot in bots(host))
        assert all(bot["team"] != 3 for bot in bots(host))
        locals_ = [player for player in rows(host) if not player["bot"] and player["peer_id"] == 255]
        run(host, f'LobbyPromoteLeader({locals_[1]["id"]});')
        time.sleep(0.2)
        run(host, f'SetControllerPlayer(0);LobbySetPlayerName({first_bot},"RED GUARD");LobbySetPlayerTeam({first_bot},2);')
        wait(host, lambda: any(bot["name"] == "RED GUARD" and bot["team"] == 2 for bot in bots(host)),
             "host who is not local leader manages bots")
        time.sleep(0.3)
        screenshot(host, "lobby_bots_host")
        screenshot(guest, "lobby_bots_guest")
        run(host, 'SetControllerPlayer(0);LobbySetSelection("com-wolfire-arena","com-wolfire-arena",'
            '"Data/Levels/arenas/Magma_Arena.xml",false);LobbySetForceTeams(true);LobbyStart();')
        for inst in (host, guest):
            loaded(inst)
            wait(inst, lambda i=inst: all(actor(i, bot) is not None for bot in bots(i)), "bot avatars replicated")
        before_views = (state(host)["local_inputs"], state(host)["local_players"], state(host)["render_views"])
        assert before_views == (2, 2, 2), before_views
        assert all(not actor(host, bot)["controlled"] for bot in bots(host))
        start_menu(host)
        action(host, "add_lobby_bot")
        wait(host, lambda: len(bots(host)) == 3, "live host adds bot")
        wait(guest, lambda: len(bots(guest)) == 3, "live bot reaches guest")
        assert before_views == (state(host)["local_inputs"], state(host)["local_players"], state(host)["render_views"])
        newest = next(bot for bot in bots(host) if bot["id"] not in retained)
        run(host, f'LobbyRemovePlayer({newest["id"]});', 0)
        wait(guest, lambda: len(bots(guest)) == 2, "live bot removed on both peers")
        assert {bot["id"] for bot in bots(host)} == retained
        metrics["online"] = {"retained_ids": sorted(retained), "host_input_player_view_counts": before_views,
                             "guest_counts": [state(guest)["local_inputs"], state(guest)["local_players"], state(guest)["render_views"]],
                             "live_add_remove": True, "host_nonleader_edit": True, "guest_denied": True}
        end_match(host, guest)
        for inst in (host, guest):
            assert {bot["id"] for bot in bots(inst)} == retained
        run(host, 'SetControllerPlayer(0);for(uint n=0;n<LobbyPlayers().length();++n)'
            'if(LobbyPlayers()[n].bot)LobbySetPlayerTeam(LobbyPlayers()[n].id,1);'
            'LobbySetForceTeams(false);LobbySetSelection("com-wolfire-overgrowth-campaign",'
            '"com-wolfire-overgrowth-campaign","og_story/01_Slaver_Camp_Landing.xml",false);LobbyStart();')
        for inst in (host, guest):
            loaded(inst)
            wait(inst, lambda i=inst: state(i)["coop_cutscene"] and all(actor(i, bot) is not None and
                 actor(i, bot)["coop_cutscene_hold"] and not actor(i, bot)["visible"] for bot in bots(i)),
                 "campaign dialogue holds and hides bots", 45)
            assert {bot["id"] for bot in bots(inst)} == retained
            assert state(inst)["render_views"] == 1
        screenshot(host, "campaign_bots_cutscene")
        run(host, 'dialogue.queue_skip=true;dialogue.Play();', target="level")
        for inst in (host, guest):
            wait(inst, lambda i=inst: not state(i)["coop_cutscene"] and all(actor(i, bot) is not None and
                 not actor(i, bot)["coop_cutscene_hold"] and actor(i, bot)["visible"] for bot in bots(i)),
                 "campaign bots resume after dialogue", 45)
        metrics["campaign"] = {"retained_ids": sorted(retained), "cutscene_hidden_held": True,
                               "resumed": True, "host_views_restored": state(host)["render_views"]}
        for inst in (host, guest):
            assert inst.bridge.call("gl_problems")["count"] == 0
        errors = [(inst.name, script_errors(inst)) for inst in all_instances if script_errors(inst)]
        return {"passed": not errors, "metrics": metrics, "notes": [str(error) for error in errors]}
    except Exception:
        return {"passed": False, "metrics": metrics, "notes": [traceback.format_exc()]}
