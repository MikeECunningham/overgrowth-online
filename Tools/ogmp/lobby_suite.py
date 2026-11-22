"""Pregame local-player rosters, authority, input mapping and compact networking.

Run ``python -I Tools/ogmp.py run local_lobby``. Set OGMP_KEYS=renderer=vulkan
to exercise Vulkan. All input uses the engine's process-local test bridge.
"""

import time

from .instance import ROOT
from .scenarios import LEVEL, dist


def local_lobby(ctx):
    metrics, failures = {}, []
    keys = {"mp_port": "26244", "gl_debug": "1"}
    mods = [ROOT + "/test/mods/online_api_test"]
    host = ctx.instance("lobby_host", keys={**keys, "playername": "Host One"}, mods=mods)
    guest = ctx.instance("lobby_guest", keys={**keys, "playername": "Guest One"}, mods=mods)

    def run(inst, code, target="menu"):
        inst.bridge.call("exec", target=target, code=code)

    def rows(inst):
        return inst.bridge.status()["online"].get("players", [])

    def wait(inst, predicate, description, timeout=30):
        return inst.bridge.wait(predicate, timeout, what=description)

    def check(condition, description):
        if not condition:
            failures.append(description)

    run(host, 'LobbyBeginHost(); LobbyAddLocalPlayer();')
    run(guest, 'LobbyAddLocalPlayer();')
    guest.bridge.call("connect", ip="127.0.0.1")
    for inst in (host, guest):
        wait(inst, lambda i=inst: len(rows(i)) == 4, "four players before loading")
        check(not inst.bridge.status()["level_loaded"], "pregame handshake loaded a world")

    initial = rows(host)
    metrics["pregame_rows"] = initial
    hp2 = next(p for p in initial if p["peer_id"] == 255 and p["local_index"] == 1)
    gp2 = next(p for p in initial if p["peer_id"] != 255 and p["local_index"] == 1)
    run(guest, f'LobbySetPlayerName({gp2["id"]}, "Guest Two"); LobbySetPlayerScreen({gp2["id"]}, 1);')
    wait(host, lambda: any(p["id"] == gp2["id"] and p["name"] == "Guest Two" and p["screen"] == 1
                          for p in rows(host)), "guest P1 changes its own names/screens")
    run(guest, f'LobbySetPlayerName({hp2["id"]}, "Stolen"); LobbySetPlayerScreen({hp2["id"]}, 2); '
               f'LobbySetPlayerTeam({hp2["id"]}, 3);')
    run(host, f'SetControllerPlayer(1); LobbySetPlayerName({hp2["id"]}, "P2 stole menu"); SetControllerPlayer(0);')
    time.sleep(0.3)
    check(next(p for p in rows(host) if p["id"] == hp2["id"])["name"] not in ("Stolen", "P2 stole menu"),
          "a guest or local P2 changed host-owned names")
    check(next(p for p in rows(host) if p["id"] == hp2["id"])["team"] == 1,
          "guest changed a team without host authority")

    # Mode selection preserves the saved manual plan; modes own effective teams.
    path = "Data/Levels/" + LEVEL
    run(host, f'LobbySetSelection("com-wolfire-arena", "", "{path}", false);')
    wait(host, lambda: [p["team"] for p in rows(host)] == [1, 1, 1, 1], "manual teams survive mode selection")
    run(host, f'LobbySetPlayerTeam({hp2["id"]}, 1); LobbyAddLocalPlayer(); LobbyAddLocalPlayer(); LobbyAddLocalPlayer();')
    for inst in (host, guest):
        wait(inst, lambda i=inst: len(rows(i)) == 7, "five host and two guest seats")
    check(next(p for p in rows(host) if p["id"] == hp2["id"])["team"] == 1,
          "joining players overwrote a host-selected team")
    check([p["team"] for p in rows(host) if p["peer_id"] == 255 and p["local_index"] >= 2] == [5, 6, 7],
          "new Arena players did not get fresh teams")
    manual = {p["id"]: p["team"] for p in rows(host)}
    run(host, f'LobbySetSelection("sandbox", "", "{path}", false);')
    wait(guest, lambda: {p["id"]: p["team"] for p in rows(guest)} == manual, "manual choices survive another mode")
    run(host, f'LobbySetPlayerTeam({gp2["id"]}, 2); LobbyStart();')
    for inst in (host, guest):
        wait(inst, lambda i=inst: i.bridge.status()["level_loaded"] and not i.bridge.status()["waiting_for_input"],
             "all peers loaded and automatically started", 180)
    time.sleep(0.5)

    # Remote inputs 4/5 existed before local P5 joined. Its real input slot must
    # append rather than overwrite a remote slot; its screen remains camera 4.
    states = {"host": host.bridge.state(), "guest": guest.bridge.state()}
    metrics["characters"] = states
    for name, inst, expected in (("host", host, 5), ("guest", guest, 2)):
        local = [p for p in states[name] if p["controlled"] and not p["remote"]]
        check(len(local) == expected and len(states[name]) == 7, f"{name} has the wrong local or total unit count")
        check(len({p["controller_id"] for p in states[name]}) == 7, f"{name} has colliding input slots")
        status = inst.bridge.status()
        check(status["local_inputs"] == expected, f"{name} input count includes remote units")
        check(not inst.bridge.call("nan_scan").get("characters"), f"{name} has non-finite unit state")
        check(status.get("gl_problems", 0) + status.get("vk_problems", 0) == 0, f"{name} renderer reported problems")
    hp5 = next(p for p in states["host"] if not p["remote"] and p["camera_id"] == 4)
    check(hp5["controller_id"] >= 6, "local P5 overwrote a previously allocated remote input")
    guest_p2 = next(p for p in states["guest"] if not p["remote"] and p["controller_id"] == 1)
    check(guest_p2["camera_id"] == 0, "screen reordering changed input ownership")

    # Default pause rights belong to the overall hosting P1.
    run(guest, 'SetControllerPlayer(1); LobbySetPaused(true); SetControllerPlayer(0);', "level")
    run(host, 'SetControllerPlayer(1); LobbySetPaused(true); SetControllerPlayer(0);', "level")
    time.sleep(0.3)
    check(not host.bridge.status()["paused"], "non-host-P1 paused without permission")
    run(host, 'LobbySetPaused(true);', "level")
    for inst in (host, guest):
        wait(inst, lambda i=inst: i.bridge.status()["paused"], "host P1 pauses all peers")
    run(host, 'LobbySetPaused(false);', "level")
    for inst in (host, guest):
        wait(inst, lambda i=inst: not i.bridge.status()["paused"], "host P1 resumes all peers")

    for name, sender, receiver, unit in (("host_p5", host, guest, hp5), ("guest_p2", guest, host, guest_p2)):
        common = unit["common_id"]
        before = next(p for p in sender.bridge.state() if p["common_id"] == common)["pos"]
        sender.bridge.call("input", controller=unit["controller_id"], hold={"up": 1.0}, steps=180)
        time.sleep(1.6)
        sender.bridge.call("input", controller=unit["controller_id"], release_all=True)
        time.sleep(0.4)  # allow the remote interpolation buffer to settle
        after = next(p for p in sender.bridge.state() if p["common_id"] == common)["pos"]
        other = next(p for p in receiver.bridge.state() if p["common_id"] == common)["pos"]
        metrics[name] = {"moved_m": dist(before, after), "replication_error_m": dist(after, other)}
        check(metrics[name]["moved_m"] > 1, f"{name} did not respond to its own input")
        check(metrics[name]["replication_error_m"] < 2, f"{name} did not replicate to the other machine")

    # Script recipients are logical players, and delivery goes to their peer.
    def route(tag, sender, recipient, want):
        for inst in (host, guest):
            inst.bridge.call("level_messages")
        run(sender, f'Online_SendToPlayer({recipient}, "apitest:msg", "{tag}");', "level")
        time.sleep(0.7)
        got = [name for name, inst in (("host", host), ("guest", guest))
               if any(line.endswith(tag) and line.startswith("apitest got ")
                      for line in inst.bridge.call("level_messages", prefix="apitest")["messages"])]
        metrics[tag] = got
        check(got == [want], f"script message {tag} delivered to {got}, expected {want}")

    route("to_guest_p2", host, gp2["id"], "guest")
    route("to_host_p2", guest, hp2["id"], "host")
    route("to_own_p2", guest, gp2["id"], "guest")
    route("to_host_primary", guest, 0, "host")
    route("to_own_host_primary", host, 0, "host")
    for name, inst in (("host", host), ("guest", guest)):
        status = inst.bridge.status()
        metrics[name + "_online"] = status["online"]
        check(status["online"]["records_in"] > 0, f"{name} compact snapshots were not received")
        ctx.shot(inst, "playing")

    guest.stop()
    wait(host, lambda: len(rows(host)) == 5, "disconnect removes every unit on the departed peer")
    check(len([p for p in host.bridge.state() if p["is_player"]]) == 5, "departed peer left orphan units")
    metrics["remaining_host_rows"] = rows(host)
    return {"passed": not failures, "metrics": metrics, "notes": failures}


def lobby_lifecycle(ctx):
    """Remove seats, keep personal settings, play twice on one connection, kick a peer."""
    metrics = {}
    keys = {"mp_port": "26644", "gl_debug": "1"}
    host = ctx.instance("lobby_lifecycle_host", keys={**keys, "playername": "Host"})
    guest = ctx.instance("lobby_lifecycle_guest", keys={**keys, "playername": "Guest"})

    def run(inst, code, target="menu"):
        inst.bridge.call("exec", target=target, code=code)

    def status(inst):
        return inst.bridge.status()

    def rows(inst):
        return status(inst)["online"].get("players", [])

    def wait(inst, predicate, description, timeout=30):
        inst.bridge.wait(predicate, timeout, what=description)

    def personal_config(inst):
        run(inst, "SaveConfig();")
        with open(inst.writedir + "Data/config.txt", encoding="utf-8") as stream:
            return dict(line.rstrip("\n").split(": ", 1) for line in stream if ": " in line)

    def start_match():
        run(host, "LobbyStart();")
        for inst in (host, guest):
            wait(inst, lambda i=inst: status(i)["level_loaded"] and not status(i)["waiting_for_input"],
                 "all peers start the match", 180)
        time.sleep(0.5)

    def end_match():
        run(host, "LobbyEndGame();", "level")
        for inst in (host, guest):
            wait(inst, lambda i=inst: status(i)["online"]["active"] and
                 status(i)["online"]["lobby_waiting"] and not status(i)["level_loaded"],
                 "return to lobby without disconnecting", 60)

    run(host, "LobbyBeginHost(); LobbyAddLocalPlayer(); LobbyAddLocalPlayer();")
    run(guest, "LobbyAddLocalPlayer(); LobbyAddLocalPlayer();")
    guest.bridge.call("connect", ip="127.0.0.1")
    for inst in (host, guest):
        wait(inst, lambda i=inst: len(rows(i)) == 6, "six pregame seats")
    hp1, hp2, hp3 = [p for p in rows(host) if p["peer_id"] == 255]
    gp1, gp2, gp3 = [p for p in rows(host) if p["peer_id"] != 255]
    run(host, f'LobbySetPlayerName({hp3["id"]}, "Host Three"); LobbySetPlayerScreen({hp3["id"]}, 1); '
              f'SetConfigValueFloat("gamepad_2_look_sensitivity", 3.3f); LobbyRemovePlayer({hp2["id"]});')
    wait(host, lambda: len(rows(host)) == 5, "host removes its middle local seat")
    run(guest, f'LobbySetPlayerName({gp3["id"]}, "Guest Three"); LobbySetPlayerScreen({gp3["id"]}, 1); '
               'SetConfigValueFloat("gamepad_2_look_sensitivity", 4.4f);')
    wait(host, lambda: any(p["id"] == gp3["id"] and p["name"] == "Guest Three" for p in rows(host)),
         "guest edits its own seat")
    run(host, f'LobbyRemovePlayer({gp2["id"]});')
    for inst in (host, guest):
        wait(inst, lambda i=inst: len(rows(i)) == 4, "host removes a remote extra seat")
    for inst, player, name, sensitivity in ((host, hp3, "Host Three", 3.3), (guest, gp3, "Guest Three", 4.4)):
        seat = next(p for p in rows(inst) if p["id"] == player["id"])
        assert seat["local_index"] == 1 and seat["screen"] == 1 and seat["name"] == name, seat
        assert status(inst)["local_inputs"] == 2
        assert abs(float(personal_config(inst)["gamepad_1_look_sensitivity"]) - sensitivity) < 0.001
    metrics["after_removal"] = rows(host)

    # Authority follows the menu owner, independent of which settings profile is displayed.
    run(host, f'SetConfigValueInt("controller_menu_owner", 1); LobbyRemovePlayer({hp3["id"]}); '
              'SetConfigValueInt("controller_menu_owner", 0); LobbyRemovePlayer(255);')
    run(guest, f'LobbyRemovePlayer({hp3["id"]});')
    time.sleep(0.2)
    assert len(rows(host)) == 4, "P2, a guest, or the host removed a protected seat"
    run(host, f'LobbySetSelection("sandbox", "", "Data/Levels/{LEVEL}", false); '
              f'LobbySetPlayerTeam({gp3["id"]}, 2);')
    start_match()
    before = rows(host)
    connection = status(host)["online"]["peers"][0]["conn_id"]
    characters = {"host": host.bridge.state(), "guest": guest.bridge.state()}
    metrics["first_match_characters"] = characters
    for values in characters.values():
        assert len(values) == 4 and all(p["char_path"] == "Data/Characters/turner.xml" for p in values)
    host_colors = {p["common_id"]: p["palette"] for p in characters["host"]}
    guest_colors = {p["common_id"]: p["palette"] for p in characters["guest"]}
    assert host_colors == guest_colors, "team appearance disagrees across peers"
    by_id = {p["common_id"]: p for p in characters["host"]}
    team_one = [p for p in before if p["id"] != hp1["id"] and p["team"] == 1]
    assert len(team_one) == 2
    assert by_id[team_one[0]["avatar_id"]]["palette"] == by_id[team_one[1]["avatar_id"]]["palette"]
    team_two = next(p for p in before if p["team"] == 2)
    assert by_id[team_two["avatar_id"]]["palette"] != by_id[team_one[0]["avatar_id"]]["palette"]
    run(guest, "LobbyEndGame();", "level")
    time.sleep(0.2)
    assert status(host)["level_loaded"], "a guest ended the shared match"
    end_match()
    metrics["returned_rows"] = rows(host)
    for inst in (host, guest):
        assert len(rows(inst)) == 4 and status(inst)["local_inputs"] == 2
        assert status(inst)["online"]["lobby_map"] == "Data/Levels/" + LEVEL
        assert next(p for p in rows(inst) if p["id"] == gp3["id"])["team"] == 2
    assert status(host)["online"]["peers"][0]["conn_id"] == connection
    assert [(p["id"], p["name"], p["screen"], p["team"]) for p in rows(host)] == [
        (p["id"], p["name"], p["screen"], p["team"]) for p in before]
    start_match()
    for inst in (host, guest):
        assert len(inst.bridge.state()) == 4
        assert status(inst).get("gl_problems", 0) + status(inst).get("vk_problems", 0) == 0
    assert status(host)["online"]["peers"][0]["conn_id"] == connection
    metrics["second_match_connection"] = connection
    end_match()
    run(guest, f'LobbyRemovePlayer({gp3["id"]});')
    for inst in (host, guest):
        wait(inst, lambda i=inst: len(rows(i)) == 3, "guest removes its own added seat")
    run(host, f'LobbyRemovePlayer({gp1["id"]});')
    wait(host, lambda: len(rows(host)) == 2 and not status(host)["online"]["peers"], "host kicks a remote primary peer")
    wait(guest, lambda: not status(guest)["online"]["active"], "kicked client disconnects")
    metrics["remaining_host_rows"] = rows(host)
    return {"passed": True, "metrics": metrics, "notes": []}
