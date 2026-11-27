"""Host-local profiles, independent mod layers, cosmetics and durable clear awards."""
import json
import shutil
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _rows, _ready
from .scenarios import script_errors


def player_profiles(ctx):
    fixture = Path(ctx.out_dir, "session-rules")
    shutil.copytree(Path(__file__).resolve().parents[2] / "test/mods/session-rules", fixture)
    manifest = fixture / "mod.xml"
    manifest.write_text(manifest.read_text().replace("<Session network_version", """
        <ModSettings><Setting key="style" label="Record style" type="choice" default="0">
          <Choice>Classic</Choice><Choice>Alternate</Choice></Setting></ModSettings>
        <ProfileLayers><SubProfile id="alternate" active_when="style"/><SubProfile id="history"/></ProfileLayers>
        <Session network_version"""))
    host = ctx.instance("profiles_host", mods=[str(fixture)], keys={**RENDER_KEYS, "mp_port": "29887"})
    guest = ctx.instance("profiles_guest", mods=[str(fixture)], keys={**RENDER_KEYS, "mp_port": "29888"})
    def wait(fn, label):
        return host.bridge.wait(fn, 60, step=.12, what=label)
    def ok(peer, expr):
        assert _probe(peer, expr) == "true", expr
    def db(peer):
        return json.loads(Path(peer.writedir, "Data/player_profiles.json").read_text())
    def profile(peer, name):
        return json.loads(_probe(peer, f'ProfileView("{name}")'))
    def catalog(peer):
        return json.loads(_probe(peer, 'ProfileSettingsView()'))

    # A profile created with the mod active owns both layers from birth.
    ok(host, 'ProfileEdit("create","Turner One","","")')
    ok(host, 'ProfileEdit("create","Cobus Two","","")')
    ok(guest, 'ProfileEdit("create","Private Guest","","")')
    ok(host, 'ProfileSetModSetting("session-rules","style",1)')
    ok(host, 'ProfileEdit("set","Turner One","outfit.rabbit","civ")')
    ok(host, 'ProfileSetModData("Turner One","session-rules","history","[12]")')
    assert _probe(host, 'ProfileGetModData("Turner One","session-rules","history")') == "[12]"
    ok(host, 'ProfileSetModSetting("session-rules","style",0)')
    assert "outfit.rabbit" not in profile(host,"Turner One")["selected"]
    ok(host, 'ProfileEdit("set","Turner One","outfit.rabbit","guard")')
    ok(host, 'ProfileSetModData("Turner One","og-online-progression","collection","{\\"test\\":2}")')
    ok(host, 'ProfileSetModSetting("og-online-progression","meta_progression",1)')
    assert "outfit.rabbit" not in profile(host, "Turner One")["selected"]
    assert _probe(host,'ProfileEdit("set","Turner One","outfit.rabbit","guard")') == "false"
    ok(host, 'ProfileSetModSetting("og-online-progression","meta_progression",0)')
    assert profile(host,"Turner One")["selected"]["outfit.rabbit"] == "guard"
    assert "collection" in db(host)["profiles"]["Turner One"]["mods"]["og-online-progression"]
    ok(host,'LobbyBeginHost()')
    guest.bridge.call("connect", ip="127.0.0.1:29887")
    wait(lambda: len(_rows(host)) == 2 and len(_rows(guest)) == 2, "guest joins")
    owner=next(row["id"] for row in _rows(host) if row["host"])
    visitor=next(row["id"] for row in _rows(host) if not row["host"])
    def lobby(peer, seat):
        return json.loads(_probe(peer, f'LobbyProfileView({seat})'))
    assert set(lobby(guest, visitor)["names"]) == {"Turner One", "Cobus Two"}
    ok(host,f'LobbyProfileEdit({owner},"select","","Turner One")')
    ok(guest,f'LobbyProfileEdit({visitor},"select","","Cobus Two")')
    wait(lambda: [p["name"] for p in _rows(host)] == ["Turner One","Cobus Two"], "both host-backed names")
    assert _probe(guest,f'LobbyProfileEdit({owner},"set","outfit.rabbit","guard2")')=="false"
    ok(guest,f'LobbyProfileEdit({visitor},"set","outfit.rabbit","guard2")')
    wait(lambda: profile(host,"Cobus Two")["selected"].get("outfit.rabbit")=="guard2","remote writes host profile")
    assert set(db(guest)["profiles"])=={"Private Guest"}
    ok(host, 'ProfileEdit("rename","Turner One","","Turner Hero")')
    wait(lambda: _rows(guest)[0]["name"]=="Turner Hero", "rename follows binding")
    _code(host, f'LobbySetSelection("survival","","{SANDBOX_MAP}",false);LobbyStart();')
    for peer in (host,guest): _ready(peer, SANDBOX_MAP)
    def actors(peer):
        by_id={a["id"]:a for a in peer.bridge.state()}
        return {row["id"]:by_id.get(row["avatar_id"],{}) for row in _rows(peer)}
    wait(lambda: all(actors(p)[owner].get("profile_outfit","").endswith("/guard.xml") and
                     actors(p)[visitor].get("profile_outfit","").endswith("/guard2.xml") for p in (host,guest)),
         "both game clients render chosen rabbit outfits")
    for p in (host, guest):
        bodies=actors(p)
        assert bodies[owner]["appearance_object"].endswith("/base_guard.xml"), bodies[owner]["appearance_object"]
        assert bodies[visitor]["appearance_object"].endswith("/IGF_Guard_reddy.xml")
        assert bodies[owner]["char_path"]==bodies[visitor]["char_path"]=="Data/Characters/turner.xml"
    # Exercise the director's publication hooks without waiting for nine
    # increasingly long batteries. Then play the tenth in a second run.
    names = ("Turner Hero", "Cobus Two")
    def wallet(name):
        return db(host)["profiles"][name]["mods"]["og-online-progression"]["collection"]
    def tokens(name):
        return wallet(name).get("tokens", 0)
    def batteries(name):
        return wallet(name).get("clear_counts", {}).get("survival_batteries", 0)
    avatars = ",".join(str(p["avatar_id"]) for p in _rows(host))
    _code(host, f"array<int> winners={{{avatars}}};ProgressionSurvivalClear(1,0,winners);", "level")
    assert all(batteries(name) == 0 for name in names)
    _code(host, 'ProfileSetModSetting("og-online-progression","meta_progression",1);', "level")
    _code(host, f"array<int> winners={{{avatars}}};for(int b=2;b<=10;++b){{"
          "ProgressionSurvivalClear(b,1,winners);ProgressionSurvivalClear(b,0,winners);"
          "ProgressionSurvivalClear(b,0,winners);}", "level")
    assert all(batteries(name) == 9 and tokens(name) == 0 for name in names)
    previous_run = host.bridge.call("progression", actor=owner)["ledger"]["profile_run"]
    _code(host, "SessionFinishSurvivalRun();Online_EndSurvivalRun();", "level")
    wait(lambda: all(not p.bridge.status()["level_loaded"] and p.bridge.status()["online"]["lobby_waiting"]
                     for p in (host, guest)), "finished Survival run returns both peers to lobby")
    _code(host, "LobbyStart();")
    for p in (host, guest): _ready(p, SANDBOX_MAP)
    assert host.bridge.call("progression", actor=owner)["ledger"]["profile_run"] != previous_run
    def enemies():
        return [c for c in host.bridge.state() if "survival_enemies" in c["teams"] and c["knocked_out"]==0 and c.get("enabled",True)]
    wait(enemies,"first wave")
    _code(host,"SessionSave(2);","level")
    saved_path = Path(host.writedir,"Data/SessionSaves/slot2.json")
    wait(saved_path.exists, "save before first clear")
    saved_mods = json.loads(saved_path.read_text())["session_mods"]
    wave_size = saved_mods["og-online-survival"]["data"]["enemies"]
    starting_completion = saved_mods["og-online-progression"]["runtime"]["profile_completion"]
    assert wave_size > 0
    def clear_saved_wave():
        # A cold restore creates one fighter per director tick. The first
        # visible opponent may be only part of the saved formation.
        def full_formation():
            live = enemies()
            return live if len(live) == wave_size else None
        for opponent in wait(full_formation, "all saved wave members deployed"):
            _code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", opponent["id"])
    clear_saved_wave()
    wait(lambda: all(tokens(name) == 1 and batteries(name) == 10 for name in names), "tenth battery on both profiles")
    for e in (owner,visitor):
        assert actors(host)[e]["profile_outfit"].endswith("/turner.xml")
    _code(host, "LobbyEndGame();", "level")
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"], "profiles in lobby")
    ok(guest,f'LobbyProfileEdit({visitor},"roll","","")')
    wait(lambda: tokens("Cobus Two")==0, "remote roll charged once")
    wait(lambda: lobby(guest,visitor).get("win"), "host sends the cosmetic reveal to its recipient")
    prize = lobby(guest,visitor)["win"]
    assert prize["serial"] and prize["option"] in lobby(guest,visitor)["unlocked"][prize["category"]]
    ok(guest,f'LobbyProfileEdit({visitor},"set","{prize["category"]}","{prize["option"]}")')
    wait(lambda: profile(host,"Cobus Two")["selected"].get(prize["category"])==prize["option"],"prize equipped")
    ok(host,'ProfileSetModSetting("og-online-progression","meta_progression",0)')
    assert profile(host,"Cobus Two")["selected"]=={"outfit.rabbit":"guard2"}
    ok(host,'ProfileSetModSetting("og-online-progression","meta_progression",1)')
    assert profile(host,"Cobus Two")["selected"][prize["category"]]==prize["option"]
    # Loading the pre-clear card preserves the original run identity. Completing
    # the same battery again cannot replace a token that was already spent.
    ok(host,"SessionLoad(2)")
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"] and
         host.bridge.call("progression",actor=owner)["ledger"]["profile_completion"] == starting_completion,
         "save restores the pre-clear ledger in the lobby")
    _code(host,"LobbyStart();")
    for p in (host,guest): _ready(p,SANDBOX_MAP)
    clear_saved_wave()
    wait(lambda: _probe(host,"int(phase)","level","level_specific")=="2", "replayed battery clears")
    assert tokens("Turner Hero")==1 and tokens("Cobus Two")==0
    assert all(batteries(name) == 10 for name in names)
    _code(host,"LobbyEndGame();","level")
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"], "back to lobby")
    # A pre-profile save also has a stable receipt identity across loads.
    card = json.loads(saved_path.read_text())
    old_ledger = card["session_mods"]["og-online-progression"]["runtime"]
    for key in ("profile_run", "profile_event", "profile_completion"):
        old_ledger.pop(key)
    saved_path.write_text(json.dumps(card))
    for _ in range(2):
        ok(host, "SessionLoad(2)")
        wait(lambda: host.bridge.call("progression", actor=owner)["ledger"]["profile_run"] == "save-" + card["image_id"],
             "pre-profile cards reopen with a repeatable receipt identity")
    # When Progression was entirely absent, map adoption initializes a fresh
    # ledger. Its clear identity must still belong to this saved card.
    del card["session_mods"]["og-online-progression"]
    saved_path.write_text(json.dumps(card))
    ok(host, "SessionLoad(2)")
    wait(lambda: host.bridge.call("progression", actor=owner)["ledger"]["profile_run"] != "save-" + card["image_id"],
         "missing module starts with a fresh ledger in the lobby")
    _code(host, "LobbyStart();")
    for peer in (host, guest): _ready(peer, SANDBOX_MAP)
    wait(lambda: host.bridge.call("progression", actor=owner)["ledger"]["profile_run"] == "save-" + card["image_id"],
         "freshly enabled Progression ties clears to this saved card")
    _code(host, "LobbyEndGame();", "level")
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"], "lobby after checking pre-mod save")
    ok(host,'ProfileEdit("delete","Turner Hero","","")')
    assert "Turner Hero" not in db(host)["profiles"] and lobby(host,owner)["guest"]
    host.stop()
    # Remove the test mod from this offline write directory. Its private
    # subprofiles must survive until the owning profile is deleted.
    shutil.rmtree(Path(host.writedir,"Data/Mods/session-rules"))
    host.start(reuse_profile=True)
    restarted = host
    assert profile(restarted,"Cobus Two")["selected"][prize["category"]]==prize["option"]
    assert batteries("Cobus Two") == 10 and tokens("Cobus Two") == 0
    assert "session-rules" in db(restarted)["profiles"]["Cobus Two"]["mods"]
    ok(restarted, 'ProfileEdit("delete","Cobus Two","","")')
    assert db(restarted)["profiles"]=={}
    assert not script_errors(restarted) and not script_errors(guest)
    return {"passed": True, "metrics": {"profiles_on_host":len(catalog(restarted)["names"]), "remote_prize":prize["option"]}, "notes":[]}


def profile_victories(ctx):
    from .mode_settings_suite import ARENA, ARENA_MAP
    from .newcampaign_local_suite import CAMPAIGN, FIRST, FIRST_ID

    host = ctx.instance("profile_clears", keys={**RENDER_KEYS, "mp_port": "29889",
                        "oo_arena_random": "false", "oo_arena_bots": "0"})
    def wait(fn, label):
        return host.bridge.wait(fn, 45, step=.1, what=label)
    def collection():
        archive = json.loads(Path(host.writedir, "Data/player_profiles.json").read_text())
        return archive["profiles"]["Raider"]["mods"]["og-online-progression"]["collection"]
    def progress():
        data = collection()
        return data.get("tokens", 0), data.get("clear_counts", {})
    _code(host, 'LobbyBeginHost(true); ProfileSetModSetting("og-online-progression","meta_progression",1);')
    owner = _rows(host)[0]["id"]
    assert _probe(host,f'LobbyProfileEdit({owner},"create","","Raider")') == "true"
    # Upgrade a wallet written before milestones existed. Its tokens and old
    # replay watermark survive, and cannot imply a particular mode's total.
    assert _probe(host, 'ProfileEdit("primary","Raider","","")')=="true"
    host.stop()
    path = Path(host.writedir, "Data/player_profiles.json")
    old_archive = json.loads(path.read_text())
    old_archive["profiles"]["Raider"]["mods"]["og-online-progression"]["collection"] = {"tokens": 3, "clears": {"old-run": 18}}
    path.write_text(json.dumps(old_archive))
    host.start(reuse_profile=True)
    _code(host, "LobbyBeginHost(true);")
    owner = _rows(host)[0]["id"]
    assert _probe(host, f'LobbyProfileEdit({owner},"select","","Raider")') == "true"
    _code(host,f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false); LobbyAddBot();')
    bot = next(row["id"] for row in _rows(host) if row["bot"])
    _code(host,f'LobbySetForceTeams(true);LobbySetPlayerTeam({owner},1);LobbySetPlayerTeam({bot},2);LobbyStart();')
    _ready(host,ARENA_MAP)
    def phase():
        return _probe(host,"int(phase)","level","level_specific")
    def knock():
        avatar=next(row["avatar_id"] for row in _rows(host) if row["bot"])
        _code(host,'SetKnockedOut(_unconscious);Ragdoll(_RGDL_LIMP);',avatar)
    wait(lambda:phase()=="2","first round begins")
    avatar = next(p["avatar_id"] for p in _rows(host) if p["id"] == owner)
    bot_avatar = next(p["avatar_id"] for p in _rows(host) if p["bot"])
    _code(host, f"array<int> defeated={{{bot_avatar}}};ProgressionArenaWin(defeated,true);ProgressionResetAttempt();", "level")
    assert progress() == (3, {})
    _code(host, f"array<int> winners={{{avatar}}};for(int n=0;n<9;++n){{ProgressionArenaWin(winners,true);"
          "ProgressionArenaWin(winners,true);ProgressionResetAttempt();}", "level")
    assert progress() == (3, {"arena_matches": 9})
    _code(host,'max_points=2;', "level", "level_specific")
    knock()
    wait(lambda:phase()=="3","first round ends")
    assert progress() == (3, {"arena_matches": 9})
    wait(lambda:phase()=="2","second round begins")
    knock()
    wait(lambda: progress() == (4, {"arena_matches": 10}), "tenth Arena match pays one token")
    _code(host,'LobbyEndGame();',"level")
    wait(lambda:host.bridge.status()["online"]["lobby_waiting"],"return for campaign")
    _code(host,f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);LobbyStart();')
    _ready(host,FIRST)
    _code(host, "for(int n=0;n<9;++n){ProgressionCampaignComplete();ProgressionCampaignComplete();ProgressionResetAttempt();}", "level")
    assert progress() == (4, {"arena_matches": 10, "campaign_levels": 9})
    _code(host,'SendGlobalMessage("levelwin");',"level")
    wait(lambda: progress() == (5, {"arena_matches": 10, "campaign_levels": 10}),
         "tenth chapter earns a separate token with XP shop off")
    assert not script_errors(host)
    host.stop()
    host.start(reuse_profile=True)
    host.bridge.call("load_level", path="set_campaign " + CAMPAIGN)
    host.bridge.call("load_level", path="load_campaign_level " + FIRST_ID)
    _ready(host, FIRST)
    wait(lambda: host.bridge.status()["online"]["local_only"] and _rows(host)[0]["name"]=="Raider",
         "direct Play adopts the saved solo profile")
    assert progress() == (5, {"arena_matches": 10, "campaign_levels": 10})
    _code(host, "for(int n=0;n<10;++n){ProgressionCampaignComplete();ProgressionResetAttempt();}", "level")
    assert progress() == (6, {"arena_matches": 10, "campaign_levels": 20})
    assert collection()["clears"]["old-run"] == 18
    return {"passed": True, "metrics": {"campaign": 20, "arena_matches": 10, "ordinary_round": 0,
            "earned_tokens": 3, "retained_tokens": 3, "solo_profile": "Raider"}, "notes": []}


def profile_intermission(ctx):
    """A funded profile never interrupts a round. Real XP shops and old save cards still work."""
    from .mode_settings_suite import ARENA, ARENA_MAP

    host = ctx.instance("profile_arena_host", width=800, height=600, keys={
        **RENDER_KEYS, "mp_port": "29891", "oo_arena_random": "false", "oo_arena_bots": "0"})
    guest = ctx.instance("profile_arena_guest", width=800, height=600, keys={**RENDER_KEYS, "mp_port": "29892"})
    archive = Path(host.writedir, "Data/player_profiles.json")
    mod = "og-online-progression"
    name = "Round Winner"

    def wait(predicate, label):
        return host.bridge.wait(predicate, 55, step=.08, what=label)

    def record():
        return json.loads(archive.read_text())["profiles"][name]["mods"][mod]

    def phase():
        return int(_probe(host, "int(phase)", "level", "level_specific"))

    def lose_round():
        avatar = next(row["avatar_id"] for row in _rows(host) if row["host"])
        _code(host, 'this_mo.static_char=false;invincible=false;ko_shield=0;TakeDamage(10);', avatar)

    def lobby():
        wait(lambda: all(p.bridge.status()["online"]["lobby_waiting"] and not p.bridge.status()["level_loaded"]
                         for p in (host, guest)), "both peers in lobby")

    assert _probe(host, f'ProfileEdit("create","{name}","","")') == "true"
    host.stop()
    saved_profiles = json.loads(archive.read_text())
    saved_profiles["profiles"][name]["mods"][mod]["collection"]["tokens"] = 256
    archive.write_text(json.dumps(saved_profiles))
    host.start(reuse_profile=True)
    assert _probe(host, "LobbyBeginHost()") == "true"
    for setting in ("meta_progression", "upgrade_progression"):
        assert _probe(host, f'ProfileSetModSetting("{mod}","{setting}",1)') == "true"
    guest.bridge.call("connect", ip="127.0.0.1:29891")
    wait(lambda: len(_rows(host)) == len(_rows(guest)) == 2, "two lobby seats")
    leader, winner = (next(p["id"] for p in _rows(host) if p["host"] == expected) for expected in (True, False))
    assert _probe(guest, f'LobbyProfileEdit({winner},"select","","{name}")') == "true"
    wait(lambda: any(p["name"] == name for p in _rows(host)), "remote profile on host")
    _code(host, f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);'
          f'LobbySetForceTeams(true);LobbySetPlayerTeam({leader},1);LobbySetPlayerTeam({winner},2);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_timer",0);LobbyStart();')
    for peer in (host, guest):
        _ready(peer, ARENA_MAP)
    wait(lambda: phase() == 2, "opening fight")
    _code(host, "max_points=2;", "level", "level_specific")
    lose_round()
    wait(lambda: phase() == 3, "first victory camera")
    # Recreate the old roll-only save from a genuine first-round victory.
    _code(host, "SessionSave(2);", "level")
    card = Path(host.writedir, "Data/SessionSaves/slot2.json")
    wait(card.is_file, "saved at the first victory")
    old = json.loads(card.read_text())
    old["world"]["arena"]["phase"] = 7
    ledger = old["session_mods"][mod]["runtime"]
    ledger.update(open=True, rolls_only=True, ready=[winner])
    card.write_text(json.dumps(old))
    wait(lambda: phase() == 2, "second round starts without any input")
    assert record()["collection"]["tokens"] == 256
    assert not record()["collection"].get("clear_counts")
    _code(host, "SessionLoad(2);", "level")
    lobby()
    _code(host, "LobbyStart();")
    for peer in (host, guest):
        _ready(peer, ARENA_MAP)
    wait(lambda: phase() == 2, "older roll-only card resumes round two")
    assert _probe(host, "round_number", "level", "level_specific") == "2"
    lose_round()
    wait(lambda: phase() == 6, "genuine match-end XP shop")
    for peer, seat in ((host, leader), (guest, winner)):
        view = wait(lambda: peer.bridge.call("progression", actor=seat)["view"], "shop view")
        wait(lambda: peer.bridge.call("progression", actor=seat)["view"].get("shop_open"), "synchronized shop")
        assert view["shop_enabled"] and "profile_rolls" not in view
    assert not host.bridge.call("progression", actor=leader, action="roll_upgrade")["submitted"]
    assert record()["collection"]["clear_counts"] == {"arena_matches": 1}
    assert record()["collection"]["tokens"] == 256

    _code(host, "LobbyEndGame();", "level")
    lobby()
    # The remote seat can spend its banked tokens on either collection.
    results = {}
    for action, layer in (("roll_upgrade", "upgrades"), ("roll", "collection")):
        def count():
            owned = record()[layer].get("owned", {})
            return len(owned) if layer == "upgrades" else sum(len(options) for options in owned.values())
        initial = count()
        balance = record()["collection"]["tokens"]
        assert _probe(guest, f'LobbyProfileEdit({winner},"{action}","","")') == "true"
        wait(lambda: count() == initial+1, "remote lobby roll grants one unowned item")
        # Exhaust the same named profile in Settings without flooding the lobby transport.
        _code(host, f'for(int i=0;i<256;++i){{if(!ProfileEdit("{action}","{name}","",""))break;}}')
        spent = balance - record()["collection"]["tokens"]
        assert count() - initial == spent and spent > 1
        assert _probe(host, f'ProfileEdit("{action}","{name}","","")') == "false"
        assert record()["collection"]["tokens"] == balance - spent > 0
        results[layer] = spent
    assert {"health", "bone_damage", "flesh_damage", "fall_resistance", "recruit", "dagger"} <= record()["upgrades"]["owned"].keys()
    assert all(not script_errors(p) for p in (host, guest))
    return {"passed": True, "metrics": results, "notes": []}
