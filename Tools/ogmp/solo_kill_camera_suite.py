"""Last-enemy slow motion and the optional Overhaul finisher in actual multiplayer rosters."""
from pathlib import Path
import time

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP
from .newcampaign_local_suite import _code, _ready, _rows
from .scenarios import script_errors


def solo_kill_camera(ctx, overhaul=False):
    mods = [str(Path(__file__).resolve().parents[2] / "mod/overgrowth-online")] if overhaul else []
    name = "overhaul_finisher" if overhaul else "solo_kill_camera"
    port = 29137 if overhaul else 29139
    keys = {**RENDER_KEYS, "global_time_scale_mult": "1", "difficulty_scaling": "false",
            "player_invincible": "true", "ocam_enabled": "true", "ofin_mode": "1",
            "ofin_length": "1", "ofin_slowmo_amt": "0.9", "tutorials": "false"}
    host = ctx.instance(name + "_host", mods=mods, keys={**keys, "mp_port": str(port)})
    bridge = host.bridge
    metrics = {}

    def slot(action, **kwargs):
        result = bridge.call("session_slots", action=action, **kwargs)
        assert result["submitted"], result
        return result

    def quiet_scene():
        host_id = next(p["avatar_id"] for p in _rows(host) if p["host"])
        for c in bridge.state():
            if c["id"] != host_id and not c["controlled"]:
                _code(host, "this_mo.static_char=true;invincible=true;", c["id"])

    def enemy():
        x, y, z = bridge.me()["pos"]
        return bridge.call("spawn", teams="finisher_enemy", pos=[x+2.0, y+.15, z])["object_id"]

    def kill(label, expected, join_during=False, threat=None, observing_guest=None):
        # Exercise the character's real damage -> defeat event, including the regular death message.
        # All other hostiles are inert except for the second-threat case below.
        player = next(p["avatar_id"] for p in _rows(host) if p["host"])
        bridge.wait(lambda: bridge.status()["time_scale"] > .97, 6, what="normal speed before " + label)
        victim = enemy()
        set_threat = f'ReadCharacterID({threat}).Execute("SetChaseTarget({player});goal=_attack;");' if threat is not None else ""
        if join_during:
            bridge.call("virtual_pad", action="attach", slot=0)
            time.sleep(.15)
        _code(host, set_threat + f"SetChaseTarget({player}); goal=_attack; attacked_by_id={player}; "
              "invincible=false; ko_shield=0; TakeBloodDamage(10);", victim)
        minimum = 1.0
        saw_camera = False
        joined = False
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            minimum = min(minimum, bridge.status()["time_scale"])
            if overhaul:
                c = next(c for c in bridge.state() if c["id"] == player)
                saw_camera |= c["ofin_victim"] == victim and c["ofin_weight"] > .2
            if observing_guest is not None:
                assert observing_guest.bridge.status()["time_scale"] > .97, "The network guest's simulation slowed"
                if overhaul:
                    assert observing_guest.bridge.me()["ofin_weight"] == 0, "The network guest entered the finisher"
            if join_during and not joined and minimum < .7 and (not overhaul or saw_camera):
                # Add a longer layer only after observing the real shot. This distinguishes roster
                # cancellation from natural expiry even if a low-frame-rate host processes input late.
                _code(host, "TimedSlowMotion(0.1f, 5.0f, 0.0f);", player)
                join_time = time.monotonic()
                bridge.call("virtual_pad", action="button", slot=0, button=0, down=True)
                bridge.wait(lambda: bridge.status()["local_players"] == 2, 2, step=.02, what="controller joins")
                bridge.call("virtual_pad", action="button", slot=0, button=0, down=False)
                bridge.wait(lambda: bridge.status()["time_scale"] > .97, 1, step=.02,
                            what="normal speed after local join")
                elapsed = time.monotonic() - join_time
                assert elapsed < 5.0, ("slow motion ended after the layer expired", elapsed)
                if overhaul:
                    def camera_stopped():
                        camera = next(c for c in bridge.state() if c["id"] == player)
                        return camera["ofin_victim"] == -1 and camera["ofin_weight"] == 0
                    bridge.wait(camera_stopped, .3, step=.02, what="camera released after local join")
                metrics["join_to_normal_speed_seconds"] = round(elapsed, 3)
                joined = True
            time.sleep(.015)
        assert next(c for c in bridge.state() if c["id"] == victim)["knocked_out"] != 0, label
        assert (minimum < .7) == expected, (label, minimum, saw_camera)
        if join_during:
            assert joined, "The controller did not join during the finisher"
        if overhaul:
            assert saw_camera == expected, (label, minimum, saw_camera)
        metrics[label] = {"minimum_speed": round(minimum, 3), "camera_blended": saw_camera} if overhaul else round(minimum, 3)

    _code(host, 'LobbyBeginHost();', "menu")
    slot("add")  # A bot and an unoccupied player reservation must never count as human players.
    reserved = slot("add")["created"]
    slot("kind", slot=reserved, player=True)
    _code(host, f'LobbySetSelection("sandbox","","{SANDBOX_MAP}",false);LobbyStart();', "menu")
    _ready(host, SANDBOX_MAP)
    quiet_scene()
    if overhaul:
        bridge.wait(lambda: any("mp_role host" in m for m in bridge.call("level_messages", prefix="mp_role")["messages"]),
                    8, what="Overhaul knows the network role before the kill")
    if not overhaul:
        # The stock scan considers an awake, aggressive enemy before playing the final blow.
        remaining = enemy()
        _code(host, "invincible=true;", remaining)
        kill("another_threat", False, threat=remaining)
        _code(host, "this_mo.static_char=true;", remaining)
    kill("one_human_bot_and_reservation", True)
    if overhaul:
        time.sleep(3)  # Allow the mod's intentional cooldown between shots.
    guest = ctx.instance(name + "_guest", mods=mods, keys={**keys, "mp_port": str(port+1)})
    guest.bridge.call("connect", ip="127.0.0.1:"+str(port))
    _ready(guest, SANDBOX_MAP)
    bridge.wait(lambda: sum(not p["bot"] for p in _rows(host)) == 2,
                15, what="network guest occupies a human seat")
    quiet_scene()
    kill("two_network_humans", False, observing_guest=guest)
    guest.stop()
    bridge.wait(lambda: sum(not p["bot"] for p in _rows(host)) == 1,
                20, what="network guest leaves")
    quiet_scene()
    kill("solo_after_guest_leaves", True, join_during=True)
    metrics["active_finisher_cancelled_on_join"] = True
    bridge.wait(lambda: sum(not p["bot"] for p in _rows(host)) == 2,
                20, what="second local human joins")
    quiet_scene()
    if overhaul:
        time.sleep(3)  # The next kill is eligible for the mod's normal cooldown.
    kill("two_local_humans", False)
    assert not script_errors(host), script_errors(host)[:4]
    return {"passed": True, "metrics": metrics, "notes": []}
