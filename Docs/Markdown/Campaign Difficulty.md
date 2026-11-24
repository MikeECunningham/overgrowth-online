# Campaign difficulty scaling

The lobby's **Difficulty mode** selects Traditional or Scaling. Open
**Difficulty** to choose the original Casual, Hardcore or Expert base and all
optional scaling factors together. These controls also appear under
**Settings → Game → Difficulty**. Traditional gates every scaling factor Off
while retaining its preferences for later use.

## Party stats and reinforcements

The host can enable **Party stats and reinforcements** in Scaling mode.
Authored species, appearances and base combat parameters are preserved. Regular
foes receive moderate durability/damage changes; bosses gain more durability
and fewer bodyguards. Human players and friendly bots keep their base stats.
A knockout does not remove a party member from the scaling count.

Each additional main-team human weighs 1.4 and each friendly bot 0.35 for stats.
Growth is linear through three weighted allies, then uses a square-root taper.
Enemy counts use a separate logarithmic curve in which an additional human
counts twice as much as a bot. Opposing lobby teams do not increase the main
party's count. Explicit authored boss/exemption parameters remain available.

Examples at four main-team humans, before progression:

| Opponent | Durability | Damage |
| --- | --- | --- |
| Ordinary rat | 1.627× | 1.261× |
| Ordinary rabbit | 1.748× | 1.261× |
| Ordinary cat/dog | 1.870× | 1.261× |
| Cat/dog boss | 3.820× | 1.418× |
| Wolf boss | 4.377× | 1.418× |

Durability changes incoming damage while preserving normalized health,
injuries, knockout shields and story health thresholds. Regular party
scaling caps at 3× durability and 1.6× damage; bosses at 6× and 1.9×.
Combined progression caps are 6× durability and 2× damage. Forced-50 can
reduce modifiers on subsequent attempts of a lost encounter.

## Physical armor and weapons

**Physical armor** equips bone-attached helmets, torso plates, shoulder guards,
forearm guards and thigh pieces. The host chooses **Armor protection** in the
lobby or under Game settings. This choice also applies to purchased gear.

| Model | How a hit resolves |
| --- | --- |
| Legacy | The original whole-body metal-armor flag handles qualifying hits. Generated or purchased coverage also grants this whole-body protection. |
| Weighted | Worn pieces contribute to one rating. A hit deals its ordinary damage divided by one plus the rating, whatever the hit location. |
| Localized | The contact point is tested against current-pose body capsules. Covered regions protect against qualifying blows or thrown-weapon penetration. |

Weighted armor counts the strongest material in each covered region once.
The base weights are 0.30 for head, 0.28 for chest, 0.20 for back, 0.095 for
each thigh, 0.09 for each shoulder and 0.065 for each forearm. Multiply by
0.50 for Wood, 0.85 for Chitin, 1.30 for Dog armor and 1.75 for Therium grey.
A Chitin chest contributes 0.238, between a Dog shoulder at 0.117 and a Dog
chest at 0.364. Authored cloth and leather use the Wood weight. An actor whose
only armor information is the original metal flag retains fallback protection.

### Armor in the installed game

The commercial Data files contain 43 model-backed object definitions in the
following armor families, referencing 26 distinct model paths. The count includes
bag, strap, scale and trim pieces that help assemble a suit. A mirrored copy on
the other limb uses the same object. The shop sells a selection of these assets.

| Asset family | Object definitions | Contents |
| --- | ---: | --- |
| Dog armor | 18 | Stock metal plates, fittings and one model containing the whole kit |
| Therium grey dog armor | 17 | Grey texture variants of the individual dog meshes |
| Therium chitin | 7 | Chest, back, head, collar, shoulder, arm and thigh |
| Rabbit wood | 1 | A wooden plate with authored limb attachments |

The standard dog and grey variant share geometry and use different color maps.
Armor can also be part of a character model. For example, the light-armored
dog's color palette includes an **Armor** channel. Lighting and character
colors affect its appearance. The stock metal gear does not specify an alloy.

Equipment intensity combines encounter pressure with a small party-size
contribution when party scaling is enabled. Armor has a delay, a cubic onset,
and a logarithmically slowing tail. Armored enemies and partial sets become
more common gradually; full sets take considerably longer. Deterministic
percentiles keep attempts consistent at the same difficulty. Each generated slot
has its own material roll, with stronger pieces appearing as equipment intensity
rises. Arms, shoulders and thighs can receive all four materials. Head, chest
and back start at Chitin. Existing campaign plates reserve the slots they cover,
including wooden plates worn through item sockets. The inner and outer plates
of a stock Dog shoulder and the segments of a Chitin backplate each count as
one protection slot.

For the armor curve, let `I` be effective encounter pressure plus `0.10` times
the softened weighted ally count, and `x = max(0, I - 0.30)`. The maturity is
`m = log(1 + x³) / (0.65 + log(1 + x³))`. An enemy receives armor with probability
`m`; once selected it gets one piece, and each remaining region has probability
`m` of receiving another. Full coverage therefore approaches gradually rather
than jumping to complete sets. At `I = 0.8`, about 15% are armored, averaging
two pieces; at `I = 1.5`, about 61% average six pieces and only about 1% have a
full set. At `I = 2.5`, about 79% are armored and 12% have full coverage.

**Weapons for extra enemies** affects generated reinforcements only. They start
unarmed; an early-middle wave can gain one dagger wielder, middle waves can
gain multiple dagger wielders, and swords/spears appear late. Each generated
fighter receives at most one scaling weapon. Wolves never receive weapons.
Authored NPC weapons are preserved.

Gear creation is spread across updates. The host replicates exact armor bone
attachments, scale and protection masks, and uses the existing item ownership
and attachment systems for weapons. Disabling armor removes only added pieces.
Retries rebuild generated gear; player-taken or dropped weapons are not refilled
or silently deleted. Friendly bots and human players receive no scaling gear.

Enemy sight and hearing ranges also scale with the durability and damage sampled
for the encounter. The range factor is `1 + 0.10 × (durability − 1) + 0.25 ×
(damage − 1)`, clamped between 0.9 and 1.5. Party or progression pressure
building during a fight takes effect at the next difficulty boundary. The
separate AI scaling option controls combat behavior.

## Optional AI scaling

Choose the **AI scaling** rate under **Difficulty → Scaling**. Off disables it. Gentle, Standard, Brisk and Nightmare multiply the variation by 0.5×, 1×, 1.5× and 2×. One in eight
stable enemy identities is eligible for one small offensive variation:
aggression, ground aggression or block follow-up. Four, eight and sixteen humans
allow 1%, 2% and 3% adjustments at Standard; later encounter pressure can unlock those same
small tiers for solo play. Defensive blocking, counters, weapon catching and
attack speed remain authored, and explicit zero/maximum values stay exact.
Friendly bots do not gain these changes. AI and physical equipment are separate
from the party-stat checkbox.

## Reinforcement and story safeguards

The first extra human or friendly bot counts toward enemy numbers at the next
checkpoint, retry or new level. The reinforcement budget grows more slowly as
the party gets larger. Let `x = extra humans + 0.5 × friendly bots` and
`L = 2 × log2(1 + x / 2)`. Round to the nearest whole enemy after applying the
following encounter weights:

- Ordinary rats/rabbits: `round(L)`, capped at **six** extras per fight.
- Stronger ordinary enemies: `round(0.85 × L)`, capped at **six**.
- Ordinary bosses: `round(0.4 × L)`, capped at **two** bodyguards.
- Large-dog bosses: `round(0.3 × L)`, capped at **one**.
- Wolf bosses: `round(0.25 × L)`, capped at **one**.

| Party | Extra rats/rabbits | Extra stronger regulars | Ordinary boss bodyguards |
| --- | --- | --- | --- |
| Two humans | 1 | 1 | 0 |
| Four humans | 3 | 2 | 1 |
| Eight humans | 4 | 4 | 2 |
| One human + one bot | 1 | 1 | 0 |
| One human + three bots | 2 | 1 | 1 |
| One human + seven bots | 3 | 2 | 1 |
| One human + fifteen bots | 4 | 4 | 2 |

This budget is independent of the original group size. Reinforcements use
existing encounter species, with the weakest existing member preferred for
bodyguards. Explicit boss markers still take precedence over inference.

There are at most **eight awake generated reinforcements at once**, across
encounters. This is a concurrent limit, not an allowance exhausted for the
rest of the chapter. Defeated reinforcements free room for later fights.
An additional limit of **32 nearby enabled, awake hostile NPCs** prevents
overcrowding; distant or future encounters do not consume that local budget.
Human characters are outside that NPC cap; hostile lobby bots consume it.
Encounters above the cap still receive stat scaling. A killed reinforcement
still consumes its original encounter allocation and is not replaced.

Waves begin with an active authored defeat objective, a nearby living MC
teammate, safe ground and space, and an eligible awake actor outside
dialogue/static staging. Reinforcements arrive in small batches to limit
creation and network spikes. A reserved wave can finish after its original
enemy is knocked out; the objective waits for those reinforcements too.
Unsafe placement or exhausted safety limits can cancel an unfinished wave
without trapping story progression. Campaign cinematics do not spawn
reinforcements. Authored objective strings and editor connections remain
unchanged, and deployed opposing seats must also be defeated before proceeding.

Enemy durability, outgoing damage, eligible AI variations and reinforcement
budgets use the party and pressure sampled at the last load, saved checkpoint
or retry. New Arena matches also take a sample. Changes to the party or scaling
settings take effect at the next boundary. In both Traditional and Scaling
mode, enemies keep the base difficulty preset from that same boundary. An
enemy that joins the fight later uses the same preset. Existing reinforcements stay in
their current fight if its budget shrinks. Completed encounters can retire distant defeated reinforcements and their
unused gear, preserving player-held loot. A retry removes generated actors and
their unused gear and rebuilds that attempt's budget; player-held loot is
preserved. Chapter transitions start fresh. New actors and scaling factors are reliably replicated, including
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
python3 -I Tools/ogmp.py run difficulty_hud campaign_equipment
python3 -I Tools/ogmp.py run campaign_scaling
python3 -I Tools/ogmp.py run campaign_teams
python3 -I Tools/ogmp.py run bot_escort_stealth
```

The native unit suite also checks solo neutrality, exact preset identity,
boss emphasis, species-aware addition counts, diminishing growth, hard caps,
and independent AI/equipment factors, deterministic coverage and weapon tiers.
It also covers overlapping faction friendships, ordered team mapping and wildcards, MC anchor
replacement, and stable balanced bot escorts.

Mode-specific time, level/match and performance rules are described in
[Mode challenge settings](Mode%20Challenge.md). Losses count complete human-party
wipes; Forced-50 provides encounter-local retry relief.
