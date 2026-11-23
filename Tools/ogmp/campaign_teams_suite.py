"""Main-character parties, faction-aligned opponents, and balanced bot escorts.

Uses real Ice Cliff teams: hero/friendlies 'a', wolf67 'b', and bridge NPC440
'a,b'. MC deliberately uses lobby team2; team1 maps to wolf's faction and
teams3/4 are hostile wildcards. Inputs affect only isolated test processes.
"""
import math
import time

from .campaign_scaling_suite import CAMPAIGN, LEVEL, FACTORS, RENDER_KEYS
from .newcampaign_local_suite import _ready, _skip
from .scenarios import script_errors


def campaign_teams(ctx):
    host = ctx.instance("teams_host", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27984",
                        "player_invincible": "true", "game_difficulty": "1", "global_time_scale_mult": "1",
                        "tutorials": "false", "auto_ledge_grab": "false", "campaign_mp_scaling": "true",
                        "campaign_mp_nightmare": "false"})
    guest = ctx.instance("teams_guest", width=640, height=360, keys={**RENDER_KEYS, "mp_port": "27985",
                         "player_invincible": "true", "texture_reduce": "3"})
    metrics = {}
    combat_fixture = False

    def code(inst, text, target="level"):
        return inst.bridge.call("exec", target=target, code=text, timeout=120)

    def rows(inst=host):
        return inst.bridge.status()["online"]["players"]

    def world(inst=host):
        return inst.bridge.state()

    def wait(predicate, name, timeout=45):
        return host.bridge.wait(predicate, timeout, step=0.15, what=name)

    def avatar(player_id, inst=host):
        row = next(p for p in rows(inst) if p["id"] == player_id)
        return next((c for c in world(inst) if c["id"] == row["avatar_id"]), None)

    def by_common(common, inst=host):
        return next((c for c in world(inst) if c["common_id"] == common), None)

    def factors(actor):
        return tuple(actor["combat_stats"].get(k, -1) for k in FACTORS)

    def probe(inst, expression):
        inst.bridge.call("level_messages", prefix="teams_probe")
        code(inst, 'level.SendMessage("teams_probe " + (' + expression + '));')
        messages = inst.bridge.call("level_messages", prefix="teams_probe")["messages"]
        assert messages, expression
        return messages[-1].split(" ", 1)[1]

    def friendly(a, b, inst=host):
        return probe(inst, f'""+ReadCharacterID({a}).OnSameTeam(ReadCharacterID({b}))') == "true"

    def party_size():
        return int(probe(host, '""+Online_CampaignPartySize()'))

    def main_member(player_id, inst=host):
        character = avatar(player_id, inst)
        return character and probe(inst, f'""+Online_CampaignIsMainCharacter({character["id"]})') == "true"

    def freeze_humans(inst):
        code(inst, 'for(int i=0;i<GetNumCharacters();++i){MovementObject@ c=ReadCharacter(i);'
             'if(c.controlled&&!c.network_proxy){c.static_char=true;c.Execute("invincible=true;");}}')

    def quiet_npcs():
        for c in world():
            if c["controlled"] or c.get("proxy"): continue
            code(host, 'combat_allowed=false;hostile=false;listening=false;always_unaware=true;'
                 'invincible=true;SetTargetID(-1);attacked_by_id=-1;'
                 'if(goal==_attack||goal==_investigate||goal==_get_help)SetGoal(_patrol);' +
                 ('this_mo.static_char=true;' if combat_fixture else ''), c["id"])

    def clear_main_combat():
        for inst in (host, guest):
            for p in rows(inst):
                c = avatar(p["id"], inst)
                if c and not c["proxy"] and p["team"] == 2:
                    code(inst, 'SetTargetID(-1);attacked_by_id=-1;SetState(_movement_state);', c["id"])

    def begin_combat(target):
        c = avatar(host_humans[1])  # Any MC teammate, not only the story actor.
        code(host, f'SetTargetID({target});SetState(_attack_state);', c["id"])

    def set_encounter(originals, advance=False):
        host.bridge.call("level_messages", prefix="teams_encounter")
        goal = "defeat " + " ".join(str(x) for x in originals)
        code(host, 'for(int i=0;i<GetNumHotspots();++i){Hotspot@ h=ReadHotspot(i);'
             'Object@ o=ReadObjectFromID(h.GetID());if(h.HasVar("progress")&&o.GetScriptParams().HasParam("goal_0")){'
             'int goal=h.GetIntVar("progress")+' + ("1" if advance else "0") + ';'
             f'o.GetScriptParams().SetString("goal_"+goal,"{goal}");'
             'o.GetScriptParams().SetString("goal_"+goal+"_pre","");'
             'o.GetScriptParams().SetString("goal_"+goal+"_post","");'
             'h.SetIntVar("progress",goal);level.SendMessage("teams_encounter "+h.GetID()+" "+goal);}}')
        messages = host.bridge.call("level_messages", prefix="teams_encounter")["messages"]
        assert len(messages) == 1, messages
        return tuple(int(v) for v in messages[0].split()[1:])

    def spawn_originals(offset=0):
        originals = []
        for i in range(3):
            pos = [base[0] + 2 + i * 0.7 + offset, base[1] + 0.2, base[2] + 2]
            ident = host.bridge.call("spawn", pos=pos, teams="b")["object_id"]
            code(host, f'ReadObjectFromID({ident}).GetScriptParams().SetInt("Campaign Boss",0);'
                 f'ReadCharacterID({ident}).ReceiveScriptMessage("set_combat_allowed false");')
            originals.append(ident)
        # Hold these authored-fixture targets in place through the long quiet
        # and reconnect windows. Ice Cliff has ledges beside the party; falling
        # targets leave combat range. Reinforcement eligibility is covered by
        # campaign_scaling rather than this opposition/ownership scenario.
        quiet_npcs()
        return originals

    def check_spawned(label):
        last_quiet = [0.0]
        def settled():
            if time.monotonic() - last_quiet[0] >= 0.8:
                quiet_npcs()
                last_quiet[0] = time.monotonic()
            hrows, grows = rows(), rows(guest)
            if len(hrows) != len(grows): return False
            for ident in opposing:
                h, g = avatar(ident), avatar(ident, guest)
                if not h or not g or not h.get("enabled", True) or not g.get("enabled", True): return False
                if h.get("knocked_out") != 0 or g.get("knocked_out") != 0: return False
                if math.dist(h["pos"], g["pos"]) > 1.5: return False
                if math.dist(h["pos"], avatar(host_humans[0])["pos"]) > 50: return False
            return True
        wait(settled, label)
        remote = avatar(guest_human, guest)
        assert remote["proxy"] is False and avatar(guest_human)["proxy"] is True, "enemy human lost input/simulation ownership"
        assert guest.bridge.status()["my_avatar"] == remote["id"] and guest.bridge.status()["render_views"] == 1
        assert host.bridge.status()["render_views"] == 2
        assert host.bridge.status()["campaign_main_avatar"] == avatar(host_humans[0])["id"], "opposition took over the story actor"
        return {p: avatar(p)["pos"] for p in opposing}

    code(host, 'LobbyBeginHost();LobbyAddLocalPlayer();for(int i=0;i<9;++i)LobbyAddBot();'
         'LobbySetSelection("' + CAMPAIGN + '","' + CAMPAIGN + '","' + LEVEL + '",false);', "menu")
    guest.bridge.call("connect", ip="127.0.0.1:27984")
    wait(lambda: len(rows()) == len(rows(guest)) == 12, "mixed campaign lobby roster")
    host_humans = [p["id"] for p in rows() if not p.get("bot") and p["peer_id"] == 255]
    host_humans.sort(key=lambda n: next(p["local_index"] for p in rows() if p["id"] == n))
    guest_human = next(p["id"] for p in rows() if not p.get("bot") and p["peer_id"] != 255)
    bots = sorted(p["id"] for p in rows() if p.get("bot"))
    friendly_bots, enemy_bots, wildcards = bots[:5], bots[5:7], bots[7:]
    enemy_bot = enemy_bots[0]
    teams = {**{p: 2 for p in host_humans + friendly_bots}, guest_human: 1, **{p: 1 for p in enemy_bots},
             wildcards[0]: 3, wildcards[1]: 4}
    for ident, team in teams.items(): code(host, f"LobbySetPlayerTeam({ident},{team});", "menu")
    code(host, 'LobbySetForceTeams(true);SetConfigValueString("difficulty_preset","Nightmare");LobbyStart();', "menu")
    _ready(host); _ready(guest); _skip(host)
    wait(lambda: all(avatar(p) and avatar(p, guest) for p in teams), "all campaign characters admitted")
    freeze_humans(host); freeze_humans(guest); quiet_npcs()
    wait(lambda: party_size() == 7, "MC humans and friendly bots count as seven participants")
    seven_participant_boss = 1.0 + 0.97 * 0.85 * (3.0 + math.sqrt(3.0))
    wait(lambda: math.isclose(factors(by_common(67))[0], seven_participant_boss, abs_tol=1e-5) and
         factors(by_common(67))[3] == 0,
         "enemy NPC scaling uses only the seven-member MC party despite an allied enemy human")
    assert all(main_member(p) for p in host_humans + friendly_bots)
    opposing = [guest_human] + enemy_bots + wildcards
    assert not any(main_member(p) for p in opposing)
    wait(lambda: all(not avatar(p).get("enabled", True) and not avatar(p, guest).get("enabled", True) for p in opposing),
         "opposing seats wait disabled on host and owner before the first combat")
    mc, enemy, wild_a, wild_b = [avatar(p)["id"] for p in [host_humans[0], guest_human] + wildcards]
    wait(lambda: friendly(enemy, 67) and friendly(enemy, 440), "mapped team inherits the authored wolf faction's associations")
    assert friendly(mc, 78) and friendly(mc, 440) and not friendly(mc, 67)
    assert not friendly(mc, enemy) and not friendly(enemy, 78)
    for wild in (wild_a, wild_b):
        assert not any(friendly(wild, other) for other in (mc, enemy, 67, 78, 440))
    assert not friendly(wild_a, wild_b)
    for p in friendly_bots:
        assert factors(avatar(p)) == (1, 1, 1, 0), ("friendly bot was scaled", avatar(p))
    metrics["factions"] = {p: {"team": teams[p], "tags": avatar(p)["teams"]} for p in teams}
    # Hold story progression while testing a downed escort leader; an unrelated
    # authored checkpoint heal must not immediately stand the test subject up.
    set_encounter([67])  # the authored wolf remains disabled and undefeated

    def balanced():
        leaders = [avatar(p)["id"] for p in host_humans if avatar(p).get("knocked_out") == 0]
        counts = {p: 0 for p in leaders}
        for ident in friendly_bots:
            actor = avatar(ident)
            if actor.get("ai_goal") != 4 or actor.get("ai_escort_id") not in counts: return False
            counts[actor["ai_escort_id"]] += 1
        return counts if max(counts.values()) - min(counts.values()) <= 1 else False
    counts = wait(balanced, "five friendly bots spread across both friendly humans")
    escorts = {p: avatar(p)["ai_escort_id"] for p in friendly_bots}
    time.sleep(1.2)
    assert escorts == {p: avatar(p)["ai_escort_id"] for p in friendly_bots}, "balanced escorts churned without a roster change"
    fallen = avatar(host_humans[1])["id"]
    code(host, "SetKnockedOut(_unconscious);", fallen)
    wait(lambda: all(avatar(p).get("ai_escort_id") != fallen for p in friendly_bots), "followers release an unconscious leader")
    # The game's body investigation may temporarily supersede escorting. Let
    # that fixture activity finish before checking its next patrol assignment.
    for ident in friendly_bots:
        actor = avatar(ident)
        if actor.get("ai_goal") not in (0, 4): code(host, "SetGoal(_patrol);", actor["id"])
    wait(lambda: all(avatar(p).get("ai_escort_id") == avatar(host_humans[0])["id"] for p in friendly_bots),
         "followers leave an unconscious leader")
    assert party_size() == 7, "a fallen MC participant reduced encounter scaling"
    code(host, "Recover();", avatar(host_humans[1])["id"])
    wait(balanced, "followers rebalance when the second leader recovers")
    metrics["followers"] = {"counts": counts, "assignments": escorts}

    combat_fixture = True
    quiet_npcs()
    base = avatar(host_humans[0])["pos"]
    first = spawn_originals()
    first_encounter = set_encounter(first)
    begin_combat(first[0])
    wait(lambda: "Campaign opposition: seat" in host.log(), "the first opponent deployment is triggered")
    clear_main_combat()
    quiet_npcs()
    # Five opposition seats exceed the per-tick placement batch of four. The
    # fifth must finish even though the triggering attack has already ended.
    first_spawn = check_spawned("every opposing player and bot joins the first encounter")
    quiet_npcs(); clear_main_combat()
    metrics["first_encounter"] = {"key": first_encounter, "opponents": first_spawn}
    assert party_size() == 7

    # Fallback re-entry must work even when an authored objective has not changed.
    for ident in opposing:
        inst = guest if ident == guest_human else host
        actor = avatar(ident, inst)
        code(inst, 'this_mo.static_char=true;this_mo.position+=vec3(0,6,0);this_mo.velocity=vec3(0);FixDiscontinuity();', actor["id"])
    time.sleep(13.0)  # production combat re-arm quiet/cooldown windows
    elevated = {p: avatar(p)["pos"] for p in opposing}
    begin_combat(first[0])
    wait(lambda: all(math.dist(avatar(p)["pos"], elevated[p]) > 2 for p in opposing), "fresh combat repositions all opposing teams")
    check_spawned("re-entered combat retains remote ownership and matching poses")
    quiet_npcs(); clear_main_combat()

    # A new authored encounter revives enemy roster seats instead of adding copies.
    code(guest, "SetKnockedOut(_dead);", avatar(guest_human, guest)["id"])
    code(host, "SetKnockedOut(_dead);", avatar(enemy_bot)["id"])
    second = spawn_originals(offset=5)
    second_encounter = set_encounter(second, advance=True)
    begin_combat(second[0])
    second_spawn = check_spawned("the next encounter revives opposing human and bot seats")
    assert len(rows()) == 12, "encounters duplicated roster seats"
    metrics["second_encounter"] = {"key": second_encounter, "opponents": second_spawn}
    quiet_npcs(); clear_main_combat()

    # Direct-IP reconnect is a new transport/roster identity. Assign that new
    # seat to the opposing team through the real host controls, then ensure a
    # late opponent inherits the same NPC affiliation and waits for combat.
    old_guest = guest_human
    guest.bridge.call("disconnect")
    wait(lambda: len(rows()) == 11 and not guest.bridge.status()["online"]["active"], "enemy human leaves")
    assert party_size() == 7
    guest.bridge.call("connect", ip="127.0.0.1:27984")
    _ready(guest)
    wait(lambda: len(rows()) == len(rows(guest)) == 12, "late enemy human is readmitted")
    guest_human = next(p["id"] for p in rows() if not p.get("bot") and p["peer_id"] != 255)
    if guest_human != old_guest:
        teams.pop(old_guest)
        teams[guest_human] = 1
    opposing = [guest_human] + enemy_bots + wildcards
    code(host, f"LobbySetPlayerTeam({guest_human},1);")
    wait(lambda: avatar(guest_human) and friendly(avatar(guest_human)["id"], 67), "late opponent keeps the NPC faction association")
    wait(lambda: party_size() == 7 and not main_member(guest_human) and
         not avatar(guest_human).get("enabled", True) and not avatar(guest_human, guest).get("enabled", True),
         "new opposing seat is staged and excluded from the main party")
    freeze_humans(guest)
    quiet_npcs(); clear_main_combat()
    time.sleep(13.0)
    begin_combat(second[0])
    metrics["late_opponent"] = check_spawned("late opposing human joins the next combat with its own simulation")
    quiet_npcs(); clear_main_combat()

    # Turn off the campaign team override through the ordinary lobby controls.
    code(host, "LobbyEndGame();")
    wait(lambda: host.bridge.status()["online"]["lobby_waiting"] and guest.bridge.status()["online"]["lobby_waiting"], "party returns to lobby")
    code(host, "LobbySetForceTeams(false);LobbyStart();", "menu")
    _ready(host); _ready(guest); _skip(host)
    wait(lambda: all(avatar(p) and main_member(p) for p in teams), "ignoring lobby teams restores the cooperative party")
    mc = avatar(host_humans[0])["id"]
    assert all(friendly(mc, avatar(p)["id"]) for p in teams)
    assert party_size() == 12, "all cooperative human/bot seats should count when campaign teams are ignored"
    metrics["teams_ignored"] = {p: avatar(p)["teams"] for p in teams}
    for inst in (host, guest):
        assert not script_errors(inst), (inst.name, script_errors(inst)[:8])
        assert not inst.bridge.call("nan_scan").get("characters"), inst.name
    return {"passed": True, "metrics": metrics, "notes": []}
