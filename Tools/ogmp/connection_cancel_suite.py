"""Cancel retired/unanswered lobby connections, then reuse the client safely.

All callbacks run through the real GNS transport and connecting-menu script.
Only instances created by the supplied Context are touched.
"""

from pathlib import Path
import time


def connection_cancel(ctx):
    host_port = 27944
    host = ctx.instance("connection_cancel_host", keys={"mp_port": str(host_port)})
    client = ctx.instance("connection_cancel_client", keys={"mp_port": "27945"})
    metrics = {"cancel_seconds": []}

    def online(inst):
        assert inst.alive(), f"{inst.name} exited during connection cleanup"
        return inst.bridge.status()["online"]

    def run(inst, source):
        inst.bridge.call("exec", target="menu", code=source)

    def wait(inst, predicate, description, timeout=20):
        return inst.bridge.wait(predicate, timeout, step=0.05, what=description)

    def stopped(inst):
        state = online(inst)
        return not state["active"] and not state["peers"] and not state["players"]

    # A genuine terminal callback used to be followed by another receive pass
    # on the handle that GNS had just closed (online.cpp Invalid connection).
    run(host, "LobbyBeginHost();")
    client.bridge.call("connect", ip=f"127.0.0.1:{host_port}")
    wait(client, lambda: len(online(client)["players"]) == 2, "initial lobby admission")
    before = time.monotonic()
    host.bridge.call("disconnect")
    wait(host, lambda: stopped(host), "host teardown")
    wait(client, lambda: stopped(client), "known host closure", 10)
    metrics["host_closed_seconds"] = round(time.monotonic() - before, 3)

    # Repeat the reported workflow with a mixture of immediate and delayed
    # Cancel/back actions. Each attempt reuses the small transport handle pool.
    for cycle, delay in enumerate((0.0, 0.05, 0.2, 0.5) * 3):
        client.bridge.call("connect", ip=f"127.0.0.1:{host_port}")
        client.bridge.call("menu", script="multiplayer_menu_connecting.as")
        time.sleep(delay)
        before = time.monotonic()
        run(client, "CancelConnection();" if cycle % 2 == 0 else "CanGoBack();")
        wait(client, lambda: stopped(client), f"Cancel attempt {cycle + 1}", 5)
        # Round-trip to the main thread proves that Cancel neither crashed nor
        # left a blocked socket-thread join behind the inactive status flag.
        client.bridge.call("ping", timeout=3)
        metrics["cancel_seconds"].append(round(time.monotonic() - before, 3))

    # A no-answer deadline (GNS or the dialog) also retires its attempt. Cancel
    # while the dialog displays the failure must remain harmless.
    client.bridge.call("connect", ip=f"127.0.0.1:{host_port}")
    client.bridge.call("menu", script="multiplayer_menu_connecting.as")
    before = time.monotonic()
    wait(client, lambda: stopped(client), "unanswered lobby deadline", 25)
    metrics["no_answer_seconds"] = round(time.monotonic() - before, 3)
    run(client, "CancelConnection();")
    client.bridge.call("ping", timeout=3)

    # A subsequent valid connection must not inherit old delayed sends, peer
    # rows, or a late callback from the attempts just cancelled.
    run(host, "LobbyBeginHost();")
    client.bridge.call("connect", ip=f"127.0.0.1:{host_port}")
    for inst in (host, client):
        wait(inst, lambda inst=inst: len(online(inst)["players"]) == 2 and
             len(online(inst)["peers"]) == 1, "recovery admission")
    metrics["recovered_roster"] = online(host)["players"]
    before = time.monotonic()
    client.bridge.call("disconnect")
    wait(client, lambda: stopped(client), "recovered client leaves", 5)
    wait(host, lambda: len(online(host)["players"]) == 1 and not online(host)["peers"],
         "server removes recovered client")
    metrics["leave_seconds"] = round(time.monotonic() - before, 3)

    # Flush logs through normal owned-instance shutdown before inspecting them.
    client.stop()
    host.stop()
    for inst in (client, host):
        text = (Path(inst.dir) / "console.log").read_text(errors="replace")
        assert "Invalid connection handle:" not in text, f"{inst.name} received after its transport closed"
        assert "Attempted to close a connection without a valid online session" not in text
        assert "Segmentation fault" not in text
    metrics["cancel_attempts"] = len(metrics["cancel_seconds"])
    return {"passed": True, "metrics": metrics, "notes": []}
