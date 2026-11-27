"""Survival and campaign advance freely while named and guest profiles bank milestones."""
import json
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, SECOND, _ready, _rows
from .scenarios import script_errors


def profile_campaign_survival(ctx):
    mod = "og-online-progression"
    name = "Scout Player"
    host = ctx.instance("map_roll_host", keys={**RENDER_KEYS, "mp_port": "29938"})
    guest = ctx.instance("map_roll_guest", keys={**RENDER_KEYS, "mp_port": "29939"})
    peers = (host, guest)
    archive = Path(host.writedir, "Data/player_profiles.json")

    def wait(fn, label):
        try:
            return host.bridge.wait(fn, 55, step=.1, what=label)
        except Exception:
            snapshot = {"waiting": label}
            for p in peers:
                rows = _rows(p)
                snapshot[p.name] = {"level": p.bridge.status()["level"], "players": rows,
                                    "progression": [data(p, row["id"]) for row in rows]}
            Path(ctx.out_dir, "roll_failure.json").write_text(json.dumps(snapshot, indent=2))
            raise

    def data(peer, seat, **kwargs):
        return peer.bridge.call("progression", actor=seat, **kwargs)

    def require_pool(peer, seat, expected):
        wait(lambda: data(peer, seat)["ledger"].get("map_upgrades") == sorted(expected),
             f"{peer.name} map unlock pool {expected}")

    def profile():
        records = json.loads(archive.read_text())["profiles"]
        assert set(records) == {name}
        return records[name]["mods"][mod]

    def roll_in_lobby(peer, seat):
        def view():
            return json.loads(_probe(peer, f"LobbyProfileView({seat})"))
        wait(lambda: view().get("can_roll_upgrade"), "lobby seat can spend the banked token")
        assert _probe(peer, f'LobbyProfileEdit({seat},"roll_upgrade","","")') == "true"
        result = wait(lambda: view().get("win"), "lobby roll reaches its owner")
        assert result["kind"] == "upgrade" and result["id"] and view()["tokens"] == 0
        return result["id"]

    def assert_guest_storage():
        local = Path(guest.writedir, "Data/player_profiles.json")
        assert not local.exists() or not json.loads(local.read_text())["profiles"]

    assert _probe(host, "LobbyBeginHost()") == "true"
    assert _probe(host, f'ProfileSetModSetting("{mod}","upgrade_progression",1)') == "true"
    guest.bridge.call("connect", ip="127.0.0.1:29938")
    wait(lambda: all(len(_rows(p)) == 2 for p in peers), "both peers joined")
    local, remote = (next(p["id"] for p in _rows(host) if p["host"] == flag) for flag in (True, False))
    assert _probe(guest, f'LobbyProfileEdit({remote},"create","","{name}")') == "true"
    wait(lambda: any(p["name"] == name for p in _rows(host)), "host received remote profile")
    assert not profile()["upgrades"].get("owned")

    _code(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false);LobbyStart();')
    for p in peers:
        _ready(p, SANDBOX_MAP)
    require_pool(host, local, set())

    def raiders():
        return [c["id"] for c in host.bridge.state() if "survival_enemies" in c["teams"]
                and c["knocked_out"] == 0 and c.get("enabled", True)]

    for actor in wait(raiders, "battery one has a live wave"):
        _code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", actor)
    wait(lambda: profile()["collection"].get("clear_counts", {}).get("survival_batteries") == 1,
         "battery clear counted after victory camera")
    assert profile()["collection"].get("tokens", 0) == 0
    # Exercise the next nine clear reports through the same director hook.
    avatars = ",".join(str(row["avatar_id"]) for row in _rows(host))
    _code(host, f"array<int> winners={{{avatars}}};for(int n=2;n<=10;++n) ProgressionSurvivalClear(n,0,winners);", "level")
    wait(lambda: profile()["collection"]["tokens"] == 1, "tenth battery earns a token")
    assert _probe(host, "ProgressionShopOpen()", "level") == "false"
    for p, seat in ((host, local), (guest, remote)):
        require_pool(p, seat, set())
        assert not data(p, seat, action="roll_upgrade")["submitted"]
    assert_guest_storage()
    _code(host, "LobbyEndGame();", "level")
    wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                     for p in peers), "both peers returned to their lobby")
    first = roll_in_lobby(guest, remote)
    temporary = roll_in_lobby(host, local)
    assert set(profile()["upgrades"]["owned"]) == {first}
    expected = {first, temporary}

    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();')
    for p in peers:
        _ready(p, FIRST)
    require_pool(host, local, expected)
    _code(host, 'SendGlobalMessage("levelwin");', "level")
    for p in peers:
        _ready(p, SECOND)
    assert profile()["collection"]["clear_counts"] == {"campaign_levels": 1, "survival_batteries": 10}
    assert profile()["collection"]["tokens"] == 0
    require_pool(guest, remote, expected)
    assert_guest_storage()
    assert all(not script_errors(p) for p in peers)
    return {"passed": True, "metrics": {"battery_clears": 10, "campaign_levels": 1,
            "named_lobby_prize": first, "guest_lobby_prize": temporary}, "notes": []}
