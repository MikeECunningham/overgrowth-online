# Campaign party difficulty

The difficulty screen and Game settings have a **Scale campaign encounters for
party size** checkbox, enabled by default. It controls enemy durability, damage,
passive-block armor, and limited reinforcements. **Casual**, **Hardcore**, and
**Expert** retain their chosen AI behavior. **Nightmare** uses Expert's base
settings and adds rare, small AI variations as the MC party grows, independently
of the stat/number checkbox. One participant keeps the original encounter values.

The lobby host controls these shared match settings. Connected clients receive
the same settings, including late joiners and after a lobby host handoff.
Selecting a different preset clears Nightmare; custom changes to the Expert
base settings no longer qualify as the exact Nightmare preset.

## Scaling philosophy

- Preserve authored enemy species, appearance, equipment, and base combat
  attributes. Extra enemies are matching copies of actors already in the fight;
  rabbits never turn into wolves to increase difficulty.
- Weak-species groups gain numbers earlier. Strong enemies and bosses primarily
  gain durability, with gentler damage and passive-block increases. Scaling
  respects the strength order rat, rabbit, cat/dog, large dog, wolf.
- Treat one to two strong/elite authored opponents, and a lone large outlier
  among weaker opponents, as bosses. The active campaign objective supplies the
  encounter membership. Friendlies, human characters, and lobby bots are excluded.
- Count MC-team humans and friendly lobby bots with an admitted character,
  including knocked-out members and temporarily disconnected controllers. A defeated player does not
  make the remaining enemies weaker. Removing a party seat changes the count.
  Opposing lobby teams do not increase the scaling count. Party modifiers never
  boost lobby bots, and Nightmare uses Expert as their base. Optional mode
  progression can separately strengthen opposing bots while allies stay unchanged.
- Scale smoothly through four participants, then use diminishing growth. All stat
  increases and reinforcement counts have hard limits.

Normalized health is left intact. Durability changes incoming damage, so story
health thresholds, regeneration, and network health bars remain valid. Armor
strengthens passive blocking; it does not enable the game's metal-armor weapon
immunities or refill knockout shields. Changes never heal existing injuries.

Examples at four MC-team participants, relative to each actor's authored base:

| Opponent | Effective durability | Damage | Passive-block armor |
| --- | --- | --- | --- |
| Ordinary rat | 1.459× | 1.19125× | 1.1785× |
| Ordinary rabbit | 1.54825× | 1.19125× | 1.1785× |
| Ordinary cat/dog | 1.6375× | 1.19125× | 1.1785× |
| Cat/dog boss | 3.0655× | 1.306× | 1.3825× |
| Wolf boss | 3.4735× | 1.306× | 1.3825× |

Regular durability caps at 3× and boss durability at 6×. Damage caps at 1.6×
and 1.9× respectively; passive-block armor caps at 1.5× and 1.9×.

Nightmare leaves AI unchanged below four participants. At four or more, one in
eight enemies is eligible for one small offensive trait variation: aggression,
ground aggression, or block follow-up. The adjustment uses 1% of that trait's
remaining range at four participants, 2% at eight, and at most 3% at sixteen or
more. It changes only that trait, keeps
explicit authored zero/maximum values, and never boosts blocking, throw counters,
weapon catching, or attack speed. Selection is stable across peers and retries.
The global difficulty slider, movement speed, and game speed stay at Expert.

## Reinforcement and story safeguards

Ordinary fights add at most **four** enemies; boss encounters at most **two**
bodyguards. Ordinary rats/rabbits gain one extra per three additional allies,
starting at four participants; stronger species gain one per four allies.
This budget is independent of the original group size. Ordinary bosses gain a
guard from six participants, large dogs from eight. Large dogs and wolves receive additions later: a solo wolf boss
does not gain another wolf until twelve participants. Reinforcements use existing
encounter species, with the weakest existing member preferred for bodyguards.

There are at most **eight additions per chapter attempt**, and additions stop
when **32 enabled, awake hostile NPCs** are already present. Existing human characters are outside that hostile-NPC cap; hostile lobby bots consume
the NPC budget. Maps above the cap still
receive stat scaling. A killed reinforcement is never replaced during the
encounter, including after another player joins.

Adds require an active authored defeat objective, a nearby living MC teammate, safe
ground and space, and an eligible awake actor outside dialogue/static staging.
Campaign cinematics do not spawn reinforcements. The objective waits for its
reinforcements and deployed opposing seats to be defeated before proceeding; authored objective strings and
editor connections remain unchanged.

Stats react to party/settings changes during play. Existing reinforcements
remain in their current fight when the party shrinks or the checkbox is turned
off. A retry removes generated actors and their unused gear and rebuilds that
attempt's budget; player-held loot is preserved. Chapter transitions start a
fresh budget. New actors and scaling factors are reliably replicated, including
for late joiners and temporary NPC ownership transfers during throws/grabs.

## Campaign teams and adversaries

**Force lobby teams** enables these rules. With it off, campaign participants
remain allied and faction/adversary deployment is inactive.

The story's main character (MC) anchors the MC team, independently of later
lobby host/leader promotions. All MC teammates keep the authored MC faction
relationships. Other occupied lobby teams, sorted by team number, are assigned
in order to the level's enemy NPC faction profiles, sorted by their team tags.
If there are `n` non-MC teams and `m` enemy profiles, exactly `min(n,m)` teams
receive NPC affiliations. The rest are separate **wildcard** teams, hostile to
NPCs and every other team; members of the same wildcard team remain allies.

The engine represents factions as comma-separated tags. Sharing any tag means
friendly. Profiles such as `b`, `b,c`, and `c` remain distinct: the bridge profile
is friendly to both, while `b` and `c` need not be friendly to each other. NPC
factions are preserved. The pause Teams list names the resulting affiliations.

Opposing seats retain their own characters, controllers, screens, and current
inventory. Before their first fight they wait outside simulation, with a waiting
message on their view. When an MC-team member enters combat, all opposing seats
are revived and placed on safe ground a short distance from that member.
Placement is authoritative on the owner's machine, including remote humans;
actors become active after their new position is confirmed. Their bots notice
the MC target after deployment. Small platforms allow a closer safe placement;
if no safe space is available after a bounded search, that seat waits for the
next combat event instead of blocking story progression.

The main campaign's authored defeat goals identify encounters. A new active
encounter deploys adversaries; a member returning to combat after eight quiet
seconds can also deploy them without a goal change. This tracking is per MC
member, with a brief two-second coalescing window for companions noticing the
same fight. Other campaigns use the combat-entry fallback. Explicit lobby seats
are reused, so new encounters do not manufacture extra copies of those seats.

Opponents cannot advance MC checkpoints, receive MC checkpoint heals or revives,
anchor MC respawns, or keep the campaign running after all MC humans are down.
Cinematics still hide and hold all extra seats; only MC allies regroup beside
the story actor afterward. Opposing seats deploy again for the next combat.

Friendly bot escorts keep stable buddies and balance across eligible living,
connected humans with matching faction relationships. Missing/dead/hostile
leaders are replaced; fighting, investigating, and weapon pickup take priority
over returning to escort formation. New bots default to the MC team; couch joins
inherit their local leader's team, with explicit team dropdowns creating enemies.

Escorts take combat and crouch cues from their assigned human. Seeing or hearing
an enemy alone does not start an attack. The player attacking, recent damage, or
an enemy attacking/chasing the player or escort permits normal defensive combat;
after four quiet seconds the bot returns to following. Crouching follows the
player, with the usual pickup, body-investigation and struggle behavior taking
priority. Navigation and visibility checks remain the game's existing systems;
there is no added cover-seeking or bush-selection planner. Bots without a human
buddy and Arena combatants retain their autonomous behavior.

## Other campaigns and author overrides

Supported story campaigns using the standard character scripts receive
hostile-NPC stat scaling. Automatic
encounter/boss grouping and reinforcements use the Overgrowth campaign's
`overgrowth_level` defeat objectives. Campaigns with different objective scripts
retain stat scaling but need their own grouping integration for reinforcements.

An actor's integer **Campaign Boss** parameter can force or suppress boss
classification (`1` or `0`). **Campaign Scaling Exempt** set to `1` excludes an
actor. These overrides permit authored exceptions without changing difficulty
choices or replacing the encounter's enemy types.

## Validation

After building, close the normal game and run isolated regressions:

```sh
python3 -I Tools/ogmp.py run difficulty_ui
python3 -I Tools/ogmp.py run campaign_scaling
python3 -I Tools/ogmp.py run campaign_teams
python3 -I Tools/ogmp.py run bot_escort_stealth
```

The native unit suite also checks solo neutrality, exact preset identity,
boss emphasis, species-aware addition counts, diminishing growth, hard caps,
and the independence of Nightmare AI from stat/number scaling. It also covers
overlapping faction friendships, ordered team mapping and wildcards, MC anchor
replacement, and stable balanced bot escorts.

Mode-specific time, level/match, win, player-defeat and adaptive win rules are described
in [Mode challenge settings](Mode%20Challenge.md). They are optional and do not add AI bonuses.
