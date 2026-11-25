"""Exercise bundled gameplay mods through the Mod menu's actual activation API."""
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import _code, _probe, ARENA, ARENA_MAP
from .newcampaign_local_suite import _ready
from .scenarios import script_errors


def gameplay_mods(ctx):
    host = ctx.instance("bundled_gameplay_mods", width=1200, height=700,
                        mods=[str(Path(__file__).resolve().parents[2] / "test/mods/session-rules")],
                        keys={**RENDER_KEYS, "mp_port": "28916", "difficulty_scaling": "true",
                              "player_invincible": "true", "oo_arena_random": "false"})
    assert _probe(host, 'ModSessionFeature("scaling") && ModSessionFeature("progression")') == "true"

    def choose_features(scaling, progression, cold_start=False):
        def flag(yes):
            return "true" if yes else "false"
        _code(host, 'array<ModID>@ ids=GetModSids(); for(uint i=0;i<ids.length();++i){'
              f'if(ModGetID(ids[i])=="og-online-scaling") ModActivation(ids[i],{flag(scaling)});'
              f'if(ModGetID(ids[i])=="og-online-progression") ModActivation(ids[i],{flag(progression)});'
              '} SaveModConfig();')
        if cold_start:
            host.stop()
            host.start(reuse_profile=True)
        assert _probe(host, 'ModSessionFeature("scaling")') == flag(scaling)
        assert _probe(host, 'ModSessionFeature("progression")') == flag(progression)
        _code(host, 'LobbyBeginHost();this_ui.SendCallback("multiplayer_lobby.as");')
        host.bridge.call("menu", script="multiplayer_lobby.as")
        _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);')
        assert _probe(host, 'LobbyScalingDifficulty()') == flag(scaling)
        assert _probe(host, 'LobbySetModeSetting("challenge_time",3)') == flag(scaling)
        assert _probe(host, 'LobbySetModeSetting("progression",1)') == flag(progression)
        assert _probe(host, 'LobbySetModeSetting("session-rules.duration",2)') == 'true'
        assert _probe(host, 'LobbySetModeSetting("session-rules.duration",3)') == 'false'
        _code(host, 'array<LobbyOption>@ opts=LobbyModOptions(); string s="";'
                    'for(uint i=0;i<opts.length();i++){s+=opts[i].key+" ";} SetConfigValueString("test_mod_options",s);')
        options = _probe(host, 'GetConfigValueString("test_mod_options")').split()
        assert "session-rules.duration" in options, options
        assert ("progression" in options) == progression, options
        if not scaling and not progression:
            ctx.shot(host, "third_party_option_with_bundles_disabled")
        _code(host, 'LobbyAddBot();LobbyStart();')
        _ready(host, ARENA_MAP)
        assert _probe(host, 'LobbyGetModeSetting("session-rules.duration")', "level") == "2"
        assert _probe(host, 'LobbySetModeSetting("session-rules.duration",1)', "level") == "false"
        # Both packages and the vanilla scripts expose the same hooks. Only
        # the active scaling overlay turns the real Arena round into an encounter.
        host.bridge.wait(lambda: _probe(host, "int(phase)", "level", "level_specific") == "2",
                         30, what="Arena starts its first fight")
        match = _probe(host, 'challenge_arena_match_key!="" && challenge_arena_round_key!=""',
                       "level", "level_specific")
        assert match == flag(scaling)
        current = host.bridge.status()
        assert bool(current["progression_ui"]) == progression, (progression, current["progression_ui"])
        assert not script_errors(host), script_errors(host)
        host.bridge.call("disconnect")
        host.bridge.wait(lambda: not host.bridge.status()["online"]["active"] and not host.bridge.status()["level_loaded"],
                         30, what="leave the session before changing mods")
        host.bridge.call("menu", script="mainmenu.as")

    choose_features(False, False, cold_start=True)
    # Recompile the same level with its overlay enabled and disabled in one process.
    pid = host.pid
    choose_features(True, False)
    choose_features(False, True)
    assert host.pid == pid
    return {"passed": True, "metrics": {"default_enabled": True, "explicit_off_after_restart": True,
                                        "live_overlay_switch": True, "third_party_lobby_setting": True}, "notes": []}
