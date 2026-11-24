"""Authored coverage, live wearables and deterministic per-slot enemy armor."""
import json
import math
import time
from pathlib import Path

from .campaign_scaling_suite import RENDER_KEYS
from .newcampaign_local_suite import CAMPAIGN, FIRST, _code, _ready, _skip
from .scenarios import script_errors


def enemy_armor_integrity(ctx):
    host = ctx.instance("enemy_armor_host", width=640, height=360, keys={
        **RENDER_KEYS, "mp_port": "28571", "difficulty_scaling": "true",
        "campaign_mp_scaling": "false", "campaign_armor_scaling": "true",
        "campaign_weapon_scaling": "false", "campaign_ai_scaling": "false",
        "armor_localized": "2", "player_invincible": "true", "tutorials": "false",
    })
    guest = ctx.instance("enemy_armor_guest", width=640, height=360,
                         keys={**RENDER_KEYS, "mp_port": "28572"})
    peers = host, guest

    def wait(predicate, label, seconds=45):
        return host.bridge.wait(predicate, seconds, step=.12, what=label)

    def state(peer=host):
        return peer.bridge.call("state")

    def actor(common_id, peer=host):
        return next((character for character in state(peer)["characters"]
                     if character["common_id"] == common_id), None)

    def armor_fixture(common_id, peer=host, **arguments):
        return peer.bridge.call("armor_fixture", char=actor(common_id, peer)["id"], **arguments)

    def generated(common_id, peer=host):
        return [piece for piece in actor(common_id, peer)["armor_attachments"] if piece["region"]]

    def profiles(common_ids, peer=host):
        actors = {character["common_id"]: character for character in state(peer)["characters"]}
        return {
            common_id: sorted((piece["region"], piece["material"], piece["path"])
                              for piece in actors[common_id]["armor_attachments"] if piece["region"])
            for common_id in common_ids
        }

    def quiet():
        _code(host, 'MovementObject@ hero=ReadCharacterID(Online_ChallengeReferenceAvatar());'
              'hero.position=Online_SafeSpotNear(ReadCharacterID(363).position,vec3(0,0,1),12);'
              'hero.Execute("FixDiscontinuity();SetOnGround(true);");')
        for peer in peers:
            _code(peer, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
                  'if(Online_IsPlayerCharacter(c.GetID())){if(!c.network_proxy)c.static_char=true;}'
                  'else c.Execute("combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");}')

    _code(host, 'LobbyBeginHost();'
          f'LobbySetSelection("{CAMPAIGN}","{CAMPAIGN}","Data/Levels/{FIRST}",false);'
          'LobbySetModeSetting("challenge_performance",1);'
          'LobbySetModeSetting("challenge_performance_rate",2);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:28571")
    wait(lambda: all(len(peer.bridge.status()["online"]["players"]) == 2 for peer in peers),
         "two armor test peers")
    _code(host, 'LobbyStart();', "menu")
    for peer in peers:
        _ready(peer, FIRST)
    _skip(host)
    quiet()
    hero = host.bridge.me()

    def spawn(path, number, team="armor_integrity_enemy"):
        angle = number * 2.39996
        position = [hero["pos"][0] + 8 * math.cos(angle), hero["pos"][1] + .2,
                    hero["pos"][2] + 8 * math.sin(angle)]
        common_id = host.bridge.call("spawn", path=path, pos=position, teams=team)["object_id"]
        _code(host, f'ReadObjectFromID({common_id}).SetTranslation('
              f'Online_SafeSpotNear(ReadCharacterID(363).position,vec3(0,0,1),{number}));'
              f'ReadCharacterID({common_id}).position=ReadObjectFromID({common_id}).GetTranslation();'
              f'ReadCharacterID({common_id}).Execute("FixDiscontinuity();SetOnGround(true);");')
        wait(lambda: actor(common_id, guest) is not None, "fixture actor replicated")
        for peer in peers:
            local_id = actor(common_id, peer)["id"]
            _code(peer, f'Object@ o=ReadObjectFromID({local_id});o.SetPlayer(false);'
                  'o.GetScriptParams().SetFloat("Movement Speed",0);'
                  f'MovementObject@ c=ReadCharacterID({local_id});'
                  'c.ChangeControlScript("enemycontrol.as");c.controlled=false;'
                  'c.Execute("SetParameters();combat_allowed=false;chase_allowed=false;always_unaware=true;listening=false;");')
        return common_id

    wearer_id = spawn("Data/Objects/IGF_Characters/IGF_GuardActor.xml", 0, hero["teams"])
    for peer in peers:
        local_id = actor(wearer_id, peer)["id"]
        _code(peer, f'ReadObjectFromID({local_id}).GetScriptParams().SetInt("Campaign Scaling Exempt",1);')
    sockets = [
        ("rightlowerarm.xml", True, 8), ("rightlowerarm.xml", False, 16),
        ("rightshoulder.xml", True, 32), ("rightshoulder.xml", False, 64),
        ("rightthigh.xml", True, 128), ("rightthigh.xml", False, 256),
    ]
    wearable_item_ids = []
    expected_mask = 0
    for socket, mirrored, region in sockets:
        item_id = host.bridge.call(
            "spawn", path="Data/Items/gear/rabbit_gear/large_wood_plate.xml",
            pos=[hero["pos"][0], hero["pos"][1] + 15, hero["pos"][2]],
        )["object_id"]
        wait(lambda: any(item["common_id"] == item_id for item in state(guest)["items"]),
             "stock wearable replicated")
        result = armor_fixture(wearer_id, op="wear", item=item_id,
                               slot="Data/Items/attachments/" + socket, mirrored=mirrored)
        expected_mask |= region
        assert result["authored_regions"] == expected_mask, (socket, mirrored, region, result)
        wait(lambda: armor_fixture(wearer_id, guest, op="inspect")["authored_regions"] == expected_mask,
             "exact stock wearable socket reaches guest")
        wearable_item_ids.append(item_id)
    for peer in peers:
        result = armor_fixture(wearer_id, peer, op="inspect")
        assert result["authored_regions"] == 504, result
        assert math.isclose(result["rating"], .25, abs_tol=1e-5), result
    armor_fixture(wearer_id, op="detach", item=wearable_item_ids[0])
    wait(lambda: armor_fixture(wearer_id, guest, op="inspect")["authored_regions"] == 496,
         "runtime wearable detach reaches guest")
    for peer in peers:
        detached = armor_fixture(wearer_id, peer, op="inspect")
        assert detached["authored_regions"] == 496, detached
        assert math.isclose(detached["rating"], .25 - .065 * .5, abs_tol=1e-5), detached
        assert len(actor(wearer_id, peer)["item_connections"]) == 6, "detachment erased restoration link"
        assert len(actor(wearer_id, peer)["attachments"]) == 5, "detached wood remains physically attached"

    stock_id = spawn("Data/Objects/lugaru_campaign/turner_armored.xml", 1)
    bare_ids = [spawn("Data/Objects/IGF_Characters/IGF_GuardActor.xml", number + 2)
                for number in range(16)]
    tracked_ids = [stock_id, *bare_ids]
    authored_ids = {
        peer: {piece["common_id"] for piece in actor(stock_id, peer)["armor_attachments"]
               if not piece["region"]}
        for peer in peers
    }
    assert all(authored_ids.values())

    def wins(start, count):
        _code(host, ''.join(
            f'Online_ChallengeEncounterBegin("armor_{number}");'
            f'Online_ChallengeEncounterWin("armor_{number}");'
            for number in range(start, start + count)
        ))

    def retry():
        _code(host, 'level.SendMessage("reset");')
        _skip(host)
        quiet()

    def settled(label):
        previous = None
        stable_since = time.monotonic()
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            current = profiles(tracked_ids)
            if current != previous:
                previous = current
                stable_since = time.monotonic()
            if (time.monotonic() - stable_since > 1.6
                    and any(current[common_id] for common_id in bare_ids)
                    and current == profiles(tracked_ids, guest)):
                return current
            time.sleep(.15)
        diagnostic = {}
        for peer in peers:
            snapshot = state(peer)
            diagnostic[peer.name] = {
                "status": peer.bridge.status(),
                "profiles": profiles(tracked_ids, peer),
                "actors": [character for character in snapshot["characters"]
                           if character["common_id"] in tracked_ids],
            }
        path = Path(ctx.out_dir) / "unsettled_armor.json"
        path.write_text(json.dumps(diagnostic, indent=2), encoding="utf-8")
        raise AssertionError("armor did not settle: " + label + "; diagnostics: " + str(path))

    def assert_unique():
        for peer in peers:
            actors = {character["common_id"]: character for character in state(peer)["characters"]}
            for common_id in tracked_ids:
                character = actors[common_id]
                regions = [piece["region"] for piece in character["armor_attachments"] if piece["region"]]
                assert len(regions) == len(set(regions)), (common_id, regions)
                assert all(region > 0 and not region & (region - 1) for region in regions), regions
                mask = sum(regions)
                assert not mask & character["authored_armor_regions"], (common_id, character)
                assert character["combat_stats"]["campaign_armor_regions"] == mask, (common_id, character)
            retained_ids = {piece["common_id"] for piece in actors[stock_id]["armor_attachments"]
                            if not piece["region"]}
            assert retained_ids >= authored_ids[peer]

    wins(0, 25)
    retry()
    # Reset restores the detached editor wearable; runtime absence must never
    # grant protection, while its legitimate starting link still works on retry.
    try:
        wait(lambda: all(armor_fixture(wearer_id, peer, op="inspect")["authored_regions"] == 504
                         for peer in peers), "retry restores all six stock wearable sockets", 15)
    except Exception as exc:
        raise AssertionError({
            peer.name: {"coverage": armor_fixture(wearer_id, peer, op="inspect"),
                        "actor": actor(wearer_id, peer)}
            for peer in peers
        }) from exc
    initial_profiles = settled("initial pressure")
    assert_unique()
    selected_id = next(common_id for common_id in bare_ids if generated(common_id))
    piece = generated(selected_id)[0]
    duplicate = armor_fixture(selected_id, op="duplicate", piece=piece["id"])
    assert duplicate["generated_copies"] == 2, duplicate
    wait(lambda: all(sum(candidate["region"] == piece["region"]
                         for candidate in generated(selected_id, peer)) == 1 for peer in peers),
         "director removes duplicate generated mesh on both peers")
    assert_unique()
    # A newly authored piece must take precedence even after the director made
    # its original plan. Preserve it and retire the overlapping runtime mesh.
    overlap = armor_fixture(selected_id, op="duplicate", piece=generated(selected_id)[0]["id"], authored=True)
    overlap_region = overlap["region"]
    wait(lambda: all(actor(selected_id, peer)["authored_armor_regions"] & overlap_region
                     and not any(piece["region"] == overlap_region for piece in generated(selected_id, peer))
                     for peer in peers), "late authored armor reserves its slot")
    assert_unique()
    # The injected authored fixture now differs from its original definition.
    # Compare all unchanged actors across retries and difficulty boundaries.
    comparable_ids = [common_id for common_id in bare_ids if common_id != selected_id]
    medium_profiles = {common_id: initial_profiles[common_id] for common_id in comparable_ids}
    retry()
    repeat_profiles = settled("same pressure retry")
    assert {common_id: repeat_profiles[common_id] for common_id in comparable_ids} == medium_profiles, \
        "retry rerolled materials or coverage"
    before = profiles(comparable_ids)
    wins(25, 20)
    time.sleep(1.2)
    assert profiles(comparable_ids) == before, "live pressure changed spawned gear"
    retry()
    high_profiles = settled("higher frozen pressure")
    assert_unique()
    stronger_slots = 0
    for common_id in comparable_ids:
        old = {region: tier for region, tier, _ in medium_profiles[common_id]}
        new = {region: tier for region, tier, _ in high_profiles[common_id]}
        assert old.keys() <= new.keys(), (common_id, old, new)
        for region, tier in old.items():
            assert new[region] >= tier, (common_id, region, tier, new[region])
            stronger_slots += new[region] > tier
    assert stronger_slots > 0, "higher frozen difficulty never produced a stronger material"
    retry()
    high_retry = settled("higher pressure retry")
    assert {common_id: high_retry[common_id] for common_id in comparable_ids} == {
        common_id: high_profiles[common_id] for common_id in comparable_ids
    }, "higher-tier retry was nondeterministic"
    assert_unique()
    assert all(not script_errors(peer) for peer in peers), [script_errors(peer) for peer in peers]
    return {
        "passed": True,
        "metrics": {
            "wearable_regions": 504, "detached_regions": 496,
            "low_pieces": sum(len(pieces) for pieces in medium_profiles.values()),
            "high_pieces": sum(len(high_profiles[common_id]) for common_id in comparable_ids),
            "stronger_existing_slots": stronger_slots, "duplicate_removed": True,
        },
        "notes": [],
    }
