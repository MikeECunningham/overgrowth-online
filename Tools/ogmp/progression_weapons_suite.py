"""Selected weapons replace supplied gear; daggers stack to a usable pair on both peers."""
import json
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _probe, _ready, _rows, _skip
from .scenarios import script_errors


def progression_starting_weapons(ctx):
    host = ctx.instance("starting_weapons_host", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28631", "player_invincible": "true", "difficulty_scaling": "false"})
    guest = ctx.instance("starting_weapons_guest", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28632", "player_invincible": "true"})
    peers = (host, guest)
    wait = lambda fn, what: host.bridge.wait(fn, 35, step=.1, what=what)
    _code(host, 'LobbyBeginHost();', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:28631")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "two weapon owners in lobby")
    p1 = next(p["id"] for p in _rows(host) if p["peer_id"] == 255)
    p2 = next(p["id"] for p in _rows(host) if p["peer_id"] != 255)

    def avatar(peer, player):
        return next(p["avatar_id"] for p in _rows(peer) if p["id"] == player)

    def equipment(peer, player):
        world = peer.bridge.call("state")
        actor = next(c for c in world["characters"] if c["id"] == avatar(peer, player))
        return sorted((item["file"].split("/")[-1].lower(), item["common_id"])
                      for item in world["items"] if item["id"] in actor["weapon_slots"][:4])

    def names(peer, player):
        return [name for name, _ in equipment(peer, player)]

    def matches():
        return all(names(peer, p1) == ["dogspear.xml", "rabbit_knife.xml"] and
                   names(peer, p2) == ["rabbit_knife.xml", "rabbit_knife.xml"] for peer in peers)

    def wait_match(label):
        try:
            wait(matches, label)
        except Exception:
            Path(ctx.out_dir, "weapon_state.json").write_text(json.dumps({
                "players": {peer.name: _rows(peer) for peer in peers},
                "states": {peer.name: peer.bridge.call("state") for peer in peers},
                "ledger": host.bridge.call("progression", actor=p1)["ledger"]}, indent=2))
            raise

    def freeze():
        for peer in peers:
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                  'if(c.controlled&&!c.network_proxy)c.static_char=true;}')

    _code(host, f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_shared",0);LobbyStart();', "menu")
    for peer in peers:
        _ready(peer, FIRST)
    _skip(host)
    wait(lambda: not host.bridge.status()["coop_cutscene"], "campaign opening finishes")
    freeze()
    wait(lambda: all(_probe(host, f'ReadCharacterID({avatar(host, p)}).GetBoolVar("coop_story_loadout_ready")') == 'true'
                     for p in (p1, p2)), "campaign starter copying finishes")
    hero, ally = avatar(host, p1), avatar(host, p2)
    enemy = host.bridge.call("spawn", path="Data/Objects/IGF_Characters/IGF_GuardActor.xml",
                             pos=[0, 5, 0], teams="weapon_xp_fixture")["object_id"]
    _code(host, f'for(int k=0;k<100;++k){{ProgressionReportHit({enemy},{hero},0,true);'
          'ProgressionCheckpoint();ProgressionResetAttempt();} ProgressionBeginShop(false);')
    for peer, player in ((host, p1), (guest, p2)):
        wait(lambda peer=peer, player=player: peer.bridge.call("progression", actor=player)["view"].get("shop_open"),
             "weapon shop reaches owner")
        assert peer.bridge.call("progression", actor=player, action="buy", key="dagger")["submitted"]
    assert host.bridge.call("progression", actor=p1, action="buy", key="spear")["submitted"]
    # Seed ordinary editor-linked campaign starters, as authored levels do.
    # The host's selected full loadout replaces sword+knife. The guest adds
    # its purchased dagger to its supplied knife, in a different sheath/hand.
    _code(host, f'for(int n=0;n<2;++n){{int who=n==0?{hero}:{ally};'
          'int knife=CreateObject("Data/Items/rabbit_weapons/rabbit_knife.xml",true);'
          'ReadObjectFromID(who).AttachItem(ReadObjectFromID(knife),_at_grip,false);'
          'if(n==0){int sword=CreateObject("Data/Items/DogWeapons/DogSword.xml",true);'
          'ReadObjectFromID(who).AttachItem(ReadObjectFromID(sword),_at_sheathe,true);}}')
    starting_defaults = equipment(host, p1)
    assert [name for name, _ in starting_defaults] == ["dogsword.xml", "rabbit_knife.xml"], starting_defaults
    for peer, player in ((host, p1), (guest, p2)):
        assert peer.bridge.call("progression", actor=player, action="ready")["submitted"]
    wait_match("campaign starters replaced and dual daggers replicated")
    def starters_hidden():
        for peer in peers:
            items = {item["common_id"]: item for item in peer.bridge.call("state")["items"]}
            if any(common not in items or items[common]["enabled"] for _, common in starting_defaults):
                return False
        return True
    def wait_hidden(label):
        try:
            host.bridge.wait(starters_hidden, 8, step=.1, what=label)
        except Exception:
            Path(ctx.out_dir, "starter_state.json").write_text(json.dumps({
                "defaults": starting_defaults,
                "states": {peer.name: peer.bridge.call("state") for peer in peers}}, indent=2))
            raise
    wait_hidden("overridden authored weapons are preserved and hidden on both peers")
    campaign_gear = equipment(host, p1), equipment(host, p2)
    for _ in range(2):
        _code(host, 'level.SendMessage("reset");')
        _skip(host)
        freeze()
        wait_match("checkpoint restores exact selected and dual-dagger loadouts")
        wait_hidden("checkpoint reapplies starting-weapon replacements on both peers")
        assert (equipment(host, p1), equipment(host, p2)) == campaign_gear

    # Both physical daggers must reach distinct hands, not just appear in UI.
    _code(guest, 'if(weapon_slots[_sheathed_left]>=0)UnSheathe(_held_left,_sheathed_left);'
          'if(weapon_slots[_sheathed_right]>=0)UnSheathe(_held_left,_sheathed_right);', avatar(guest, p2))
    wait(lambda: all(len([slot for slot in next(c for c in peer.bridge.state()
                    if c["id"] == avatar(peer, p2))["weapon_slots"][:2] if slot >= 0]) == 2 for peer in peers),
         "two daggers are usable in separate hands")
    _code(host, 'LobbyEndGame();')
    wait(lambda: all(peer.bridge.status()["online"]["lobby_waiting"] and
                     not peer.bridge.status()["level_loaded"] for peer in peers), "return to lobby")
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);'
          'LobbySetModeSetting("progression",1);LobbyStart();', "menu")
    for peer in peers:
        _ready(peer, ARENA_MAP)
    wait(lambda: all(avatar(peer, player) >= 0 for peer in peers for player in (p1, p2)),
         "Arena assigns both weapon owners")
    def arena_code(code):
        return host.bridge.call("exec", target="level", context="level_specific", code=code)
    def armed():
        host.bridge.call("level_messages", prefix="weapon_arena_phase")
        arena_code('level.SendMessage("weapon_arena_phase "+int(phase));')
        messages = host.bridge.call("level_messages", prefix="weapon_arena_phase")["messages"]
        return bool(messages) and messages[-1].split()[-1] == '2'
    wait(armed, "first ordinary Arena round is armed")
    hero, ally = avatar(host, p1), avatar(host, p2)
    arena_gear = []
    for _ in range(3):
        # Exercise Arena's normal supplied-weapon and progression handoff.
        arena_code('phase=kIntro;phase_time=the_time+1000;arm_pending=false;clear_pending=false;'
              'place_pending=false;become_pending=false;DisarmAll();ClearWeapons();')
        time.sleep(.5)
        arena_code(f'roster={{ {hero},{ally} }};random_round=true;'
              'rnd_hand={"Data/Items/DogWeapons/DogSword.xml","Data/Items/rabbit_weapons/rabbit_knife.xml"};'
              'rnd_knife={true,false};rnd_wolf={false,false};'
              'ArmPlayers();ProgressionArenaLoadouts();')
        freeze()
        wait_match("Arena supplied weapons yield exact selected and dual-dagger loadouts")
        arena_gear.append((equipment(host, p1), equipment(host, p2)))
    assert all(not script_errors(peer) for peer in peers), [script_errors(peer) for peer in peers]
    return {"passed": True, "notes": [], "metrics": {"campaign": campaign_gear, "arena": arena_gear}}
