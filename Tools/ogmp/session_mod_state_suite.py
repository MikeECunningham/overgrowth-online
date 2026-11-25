"""An installed data mod can replicate and persist its own session, independently of native providers."""
import json
import shutil
import tempfile
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe
from .newcampaign_local_suite import CAMPAIGN, FIRST, _ready, _rows
from .scenarios import script_errors


def session_mod_state(ctx):
    fixture = Path("test/mods/session-rules").resolve()
    mods = [str(fixture)]

    def start(name, port, bundles):
        return ctx.instance("moddata_" + name, width=640, height=360, mods=bundles,
                            keys={**RENDER_KEYS, "mp_port": str(port), "playername": "Mod " + name})

    def get(inst, target="menu"):
        return json.loads(_probe(inst, 'SessionGetModData("session-rules")', target))

    def set_data(inst, data, target="menu"):
        encoded = json.dumps(json.dumps(data, separators=(",", ":")))
        return _probe(inst, f'SessionSetModData("session-rules",{encoded})', target) == "true"

    def card(inst, slot):
        return Path(inst.writedir, "Data/SessionSaves", f"slot{slot}.json")

    host = start("host", 28974, mods)
    _code(host, f'LobbyBeginHost();LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);')
    data = {"chapter": 7, "roster": ["lead", "guest"]}
    assert set_data(host, data)
    assert _probe(host, 'SessionSetModData("missing","{}")') == "false"
    assert _probe(host, 'SessionSetModData("session-rules","[]")') == "false"
    assert _probe(host, 'LobbySetModeSetting("session-rules.duration",2)') == "true"

    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp, "session-rules")
        shutil.copytree(fixture, copy)
        xml = copy / "mod.xml"
        xml.write_text(xml.read_text().replace('network_version="1"', 'network_version="2"'))
        wrong = start("incompatible", 28975, [str(copy)])
        wrong.bridge.call("connect", ip="127.0.0.1:28974")
        host.bridge.wait(lambda: any("mod mismatch" in line for line in host.bridge.call("build")["chat"]),
                         25, what="network schema mismatch")
        assert any("session-rules@2" in line for line in host.bridge.call("build")["chat"])
        assert len(_rows(host)) == 1
        wrong.stop()

    guest = start("guest", 28976, mods)
    guest.bridge.call("connect", ip="127.0.0.1:28974")
    guest.bridge.wait(lambda: len(_rows(guest)) == 2 and get(guest) == data, 30, what="mod data at join")
    assert not set_data(guest, {"chapter": 99})
    data["chapter"] += 1
    assert set_data(host, data)
    guest.bridge.wait(lambda: get(guest) == data, 15, what="live mod data update")
    _code(host, "LobbyStart();")
    _ready(host, FIRST)
    _ready(guest, FIRST)
    host.bridge.wait(lambda: card(host, 0).is_file(), 30, what="autosave with mod data")
    saved = json.loads(card(host, 0).read_text())
    assert saved["version"] == 2 and saved["session_mods"]["session-rules"] == {"version": "1", "data": data}
    assert saved["rules"]["mode_settings"][CAMPAIGN]["session-rules.duration"] == 2

    guest.stop()
    host.stop()
    cold = start("cold", 28977, mods)
    dest = card(cold, 0)
    dest.parent.mkdir(exist_ok=True, parents=True)
    dest.write_text(json.dumps(saved))
    for slot, broken, expected in [(1, "version", "Incompatible save version for mod session-rules"),
                                   (2, "missing", "Enable the required mod unavailable-fixture")]:
        candidate = json.loads(json.dumps(saved))
        sections = candidate["session_mods"]
        if broken == "version":
            sections["session-rules"]["version"] = "2"
        else:
            sections["unavailable-fixture"] = sections.pop("session-rules")
        card(cold, slot).write_text(json.dumps(candidate))
        assert _probe(cold, f"SessionLoad({slot})") == "false"
        assert expected in _probe(cold, "SessionSaveStatus()")
        assert not cold.bridge.status()["online"]["active"]
    assert _probe(cold, "SessionLoad(0)") == "true"
    cold.bridge.wait(lambda: cold.bridge.status()["online"]["lobby_waiting"] and get(cold) == data,
                     20, what="cold restore with custom state")
    assert _probe(cold, 'LobbyGetModeSetting("session-rules.duration")') == "2"
    data["roster"].append("edited in restored lobby")
    assert set_data(cold, data)
    _code(cold, "LobbyStart();")
    _ready(cold, FIRST)
    assert get(cold, "level") == data
    assert not script_errors(cold), script_errors(cold)
    # Old offline cards always serialized stock module defaults, even if the
    # player never used either system. Test restoring one with both switched off.
    cold.stop()
    cold.start(reuse_profile=True)
    _code(cold, 'array<ModID>@ ids=GetModSids(); for(uint i=0;i<ids.length();++i){'
          'if(ModGetID(ids[i])=="og-online-scaling" || ModGetID(ids[i])=="og-online-progression")'
          'ModActivation(ids[i],false);} SaveModConfig();')
    cold.stop()
    cold.start(reuse_profile=True)
    assert _probe(cold, '!ModSessionFeature("scaling") && !ModSessionFeature("progression")') == "true"
    legacy = json.loads(json.dumps(saved))
    legacy["version"] = 1
    legacy.pop("session_mods")
    legacy["rules"].pop("mode_settings")
    legacy["rules"]["challenge"] = {"version": 2, "modes": {}}
    legacy["rules"]["match"]["difficulty_scaling"] = "false"
    legacy["progression"] = {"v": 2, "shop": 0, "attempt": 0, "open": False, "final": False,
                             "loop": False, "ms_left": -1, "ready": [], "xp_scales": [], "players": [], "teams": []}
    for number, key, value, required in [(3, "progression", 1, "og-online-progression"),
                                         (4, "challenge_round", 4, "og-online-scaling")]:
        used = json.loads(json.dumps(legacy))
        used["rules"]["challenge"]["modes"] = {CAMPAIGN: {key: value}}
        card(cold, number).write_text(json.dumps(used))
        assert _probe(cold, f"SessionLoad({number})") == "false"
        assert required in _probe(cold, "SessionSaveStatus()")
    card(cold, 5).write_text(json.dumps(legacy))
    assert _probe(cold, "SessionLoad(5)") == "true"
    cold.bridge.wait(lambda: cold.bridge.status()["online"]["lobby_waiting"], 20, what="legacy vanilla lobby")
    _code(cold, "LobbyStart();")
    _ready(cold, FIRST)
    assert not script_errors(cold), script_errors(cold)
    return {"passed": True, "metrics": {"schema_mismatch": True, "late_join": True, "guest_read_only": True,
                                        "live_update": True, "save_version": 2, "cold_restore": True}, "notes": []}
