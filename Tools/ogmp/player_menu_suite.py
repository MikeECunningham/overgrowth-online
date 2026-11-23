"""Independent local pause/settings menus, through the native input path.

Run ``python -I Tools/ogmp.py run player_menus``.  Set
``OGMP_KEYS=renderer=vulkan`` to exercise the same checks on Vulkan.  Screenshots
retain the two/three-screen layout and simultaneous settings for visual review.
The mode/team suite separately covers menus on remote peers and lobby teardown.
"""

import math
import os
import time
import traceback

from .scenarios import LEVEL, script_errors, wait_loaded


def player_menus(ctx):
    """Single-player pause, then three independently owned local menus."""
    metrics = {}
    keys = {"master_volume": "0", "gl_debug": "1"}
    for player in (1, 2):
        keys[f"gamepad_{player}_look_sensitivity"] = "0.25"
        for axis in ("x", "y"):
            keys[f"gamepad_{player}_invert_{axis}_look"] = "false"
    inst = ctx.instance("player_menus", level=LEVEL, width=1280, height=720, keys=keys)

    def run(code, player=None):
        target = "level" if player is None else "player_menu"
        args = {} if player is None else {"local_player": player}
        return inst.bridge.call("exec", target=target, code=code, **args)

    def status():
        return inst.bridge.status()

    def wait(predicate, description, timeout=15):
        return inst.bridge.wait(predicate, timeout, what=description)

    def check(condition, description):
        if not condition:
            raise AssertionError(description)

    def start(player):
        # Engine::HandleRabbotToggleControls must see this actual input edge;
        # calling a level script's old open_menu message bypasses ownership.
        inst.bridge.call("input", controller=player, hold={"quit": 1}, steps=1)
        time.sleep(0.15)

    def action(player, name):
        run(f'imGUI.receiveMessage(IMMessage("{name}"));', player)

    def probe(player, expression):
        # Personal UIs intentionally have no world/Level binding. Transfer the
        # result through a test-only config value, then the level-message bridge.
        inst.bridge.call("level_messages", prefix="player_menu_probe")
        run('SetConfigValueString("player_menu_test_probe", "" + (' + expression + '));', player)
        run('level.SendMessage("player_menu_probe " + GetConfigValueString("player_menu_test_probe"));')
        lines = inst.bridge.call("level_messages", prefix="player_menu_probe")["messages"]
        check(bool(lines), f"P{player + 1} probe produced no result: {expression}")
        return lines[-1].split(" ", 1)[1]

    def shot(name):
        path = os.path.abspath(os.path.join(ctx.out_dir, f"player_menus_{name}.png"))
        inst.bridge.call("screenshot", path=path)
        wait(lambda: os.path.isfile(path), f"screenshot {name}")

    def positions():
        return {actor["controller_id"]: actor["pos"] for actor in inst.bridge.state() if actor["is_player"]}

    def change_option(player, key, value="true"):
        # Exercise the checkbox's actual option_changed handler and rebuild,
        # rather than modifying its config value from outside the UI.
        run('for(uint i=0;i<gui_elements.size();++i){'
            f'if(gui_elements[i].config_name=="{key}"){{'
            'IMMessage change("option_changed");change.addString(gui_elements[i].name);'
            f'change.addString("{value}");imGUI.receiveMessage(change);break;}}}}', player)

    try:
        wait_loaded(inst)
        start(0)
        wait(lambda: status()["open_menu_players"] == [0] and status()["paused"], "single-player menu pauses")
        metrics["single_player_owner"] = probe(0, 'GetPlayerMenuInputID() + " " + GetPlayerMenuLocalIndex()')
        check(metrics["single_player_owner"] == "0 0", "single-player menu belongs to another input/profile")
        window_width, window_height = map(int, probe(0, 'GetScreenWidth() + " " + GetScreenHeight()').split())
        metrics["single_player_cursor"] = status()["cursor_visible"]
        check(metrics["single_player_cursor"], "paused single player has no mouse cursor")
        run('string actions="";for(uint i=0;i<controller_items.size();++i){'
            'IMMessage@ m=controller_items[i].controllerItem.getMessage();'
            'if(m !is null)actions+=m.name+"|";}'
            'SetConfigValueString("player_menu_test_actions",actions);', 0)
        metrics["pause_actions"] = probe(0, 'GetConfigValueString("player_menu_test_actions")').split("|")
        check("Continue" in metrics["pause_actions"] and "debug_add_local_player" not in metrics["pause_actions"],
              "pause UI exposes the internal synthetic-player command")
        shot("single_player")
        action(0, "Continue")
        wait(lambda: not status()["open_menu_players"] and not status()["paused"], "closing single-player menu resumes")

        start(0)
        wait(lambda: status()["paused"], "reopened single-player menu pauses")
        # Synthetic seats use the internal automation API. The debug spawn
        # command is deliberately absent from the player-facing pause menu.
        run("int extra_player = DebugAddLocalPlayer();")
        wait(lambda: status()["local_players"] == 2 and not status()["paused"], "second player clears automatic pause")
        metrics["two_view_size"] = probe(0, 'GetScreenWidth() + " " + GetScreenHeight()')
        check(metrics["two_view_size"] == f"{window_width // 2} {window_height}", "existing menu did not resize to its half-screen")
        shot("two_views")

        run("int extra_player = DebugAddLocalPlayer();")
        wait(lambda: status()["local_players"] == 3, "third local player")
        start(1)
        start(2)
        wait(lambda: status()["open_menu_players"] == [0, 1, 2], "three simultaneous menus")
        metrics["owners"] = [probe(p, 'GetPlayerMenuInputID() + " " + GetPlayerMenuLocalIndex()') for p in range(3)]
        metrics["three_view_sizes"] = [probe(p, 'GetScreenWidth() + " " + GetScreenHeight()') for p in range(3)]
        check(metrics["owners"] == ["0 0", "1 1", "2 2"], "menu contexts share an input owner or settings profile")
        check(metrics["three_view_sizes"] == [f"{window_width // 2} {window_height // 2}"] * 3,
              "three-view menus use the full window")
        check(not status()["paused"], "local multiplayer menus paused the shared world")
        shot("three_menus")

        for player in (1, 2):
            action(player, "Settings")
        wait(lambda: all(probe(p, "personal_settings") == "true" for p in (1, 2)), "two independent settings trees")
        metrics["settings_profiles"] = [probe(p, 'current_screen + " " + SettingsProfilePlayer()') for p in (1, 2)]
        check(metrics["settings_profiles"] == ["Gamepad 1", "Gamepad 2"], "settings edit another player's gamepad")
        change_option(1, "gamepad_1_look_sensitivity", "+")
        wait(lambda: float(probe(1, 'GetConfigValueFloat("gamepad_1_look_sensitivity")')) > 0.25,
             "P2's sensitivity slider changes P2's profile")
        change_option(2, "gamepad_2_look_sensitivity", "-")
        wait(lambda: float(probe(2, 'GetConfigValueFloat("gamepad_2_look_sensitivity")')) < 0.25,
             "P3's sensitivity slider changes P3's profile")
        metrics["sensitivity"] = [float(probe(p, 'GetConfigValueFloat("gamepad_" + SettingsProfilePlayer() + "_look_sensitivity")'))
                                  for p in (1, 2)]
        change_option(1, "gamepad_1_invert_x_look")
        change_option(2, "gamepad_2_invert_y_look")
        wait(lambda: probe(1, 'GetConfigValueBool("gamepad_1_invert_x_look")') == "true" and
             probe(2, 'GetConfigValueBool("gamepad_2_invert_y_look")') == "true", "each settings checkbox changes its own profile")
        metrics["inversion"] = [probe(p, 'GetConfigValueBool("gamepad_" + SettingsProfilePlayer() + "_invert_x_look") + " " + '
                                        'GetConfigValueBool("gamepad_" + SettingsProfilePlayer() + "_invert_y_look")') for p in (1, 2)]
        check(metrics["inversion"] == ["true false", "false true"], "a player's checkbox changed another profile/axis")
        shot("independent_settings")

        start(0)
        wait(lambda: status()["open_menu_players"] == [1, 2], "P1 closes only its menu")
        metrics["keyboard_gameplay_capture"] = status()["grab_mouse"]
        check(metrics["keyboard_gameplay_capture"], "another player's menu released the gameplay mouse")
        before = positions()
        for player in (0, 1):
            inst.bridge.call("input", controller=player, hold={"up": 1}, steps=160)
        time.sleep(1.4)
        for player in (0, 1):
            inst.bridge.call("input", controller=player, release_all=True)
        after = positions()
        metrics["movement_m"] = {p: math.dist(before[p], after[p]) for p in range(3)}
        check(metrics["movement_m"][0] > 1, "P1 could not play while P2/P3 used their menus")
        check(metrics["movement_m"][1] < 0.6 and metrics["movement_m"][2] < 0.6, "menu owner also controlled its character")

        # Start backs out of settings first; Continue then closes that context.
        start(1)
        wait(lambda: probe(1, "personal_settings") == "false", "P2 backs out of settings")
        action(1, "Continue")
        wait(lambda: status()["open_menu_players"] == [2], "P2 closes only its menu")
        check(probe(2, "personal_settings") == "true", "P2 closing its menu destroyed P3's settings")
        start(2)
        action(2, "Continue")
        wait(lambda: not status()["open_menu_players"], "all private contexts close")

        start(0)
        action(0, "toggle_world_pause")
        wait(lambda: status()["paused"], "P1 explicitly pauses the local multiplayer world")
        action(0, "Continue")
        wait(lambda: not status()["open_menu_players"], "P1 closes its paused menu")
        check(status()["paused"], "closing a menu discarded an explicit multiplayer pause")
        start(2)
        check(probe(2, "LobbyCanPause()") == "false", "P3 inherited P1's pause authority")
        run('this_ui.SendCallback("toggle_pause");', 2)
        check(status()["paused"], "P3's native callback bypassed pause authority")
        metrics["explicit_pause_preserved"] = True
        start(2)
        start(0)
        action(0, "toggle_world_pause")
        wait(lambda: not status()["paused"], "P1 resumes the world")
        action(0, "Continue")
        wait(lambda: not status()["open_menu_players"], "final menu closes")

        metrics["renderer"] = status()["renderer"]
        metrics["render_problems"] = status().get("vk_problems", 0) if metrics["renderer"] == "vulkan" else inst.bridge.call("gl_problems")["count"]
        check(metrics["render_problems"] == 0, "renderer reported problems drawing independent menus")
        errors = script_errors(inst)
        check(not errors, f"script errors: {errors}")
        return {"passed": True, "metrics": metrics, "notes": []}
    except Exception:
        return {"passed": False, "metrics": metrics, "notes": [traceback.format_exc()]}
