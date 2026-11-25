# Difficulty modes and progression

With the bundled **Difficulty Scaling** mod enabled, open **Difficulty** in the lobby and choose **Traditional** or **Scaling**. The same controls are under
**Settings → Game → Difficulty** during play. Traditional uses the original
Casual, Hardcore and Expert presets. Scaling keeps the selected preset as its
base and enables optional party, physical equipment, AI and progression factors.
Each scaling category has its own rate buttons, including **AI scaling**.

The host owns shared difficulty choices. They are replicated to late joiners
and preserved through a host handoff. Fresh installations start in Traditional;
existing enabled party/Nightmare settings migrate to Scaling. Scaling also
uses the private local simulation when a solo game has no multiplayer lobby.
No network listener is opened for that private session.

## Progression rules

Time, completion and AI use Standard by default. Performance scaling starts Off.
Gentle runs at half the Standard rate, Brisk at 1.5× and Nightmare at 2×. Time counts active gameplay and excludes
loading, ready screens, upgrade shops, shared pauses, editor sessions and cinematics.

| Rule | Standard behavior |
| --- | --- |
| Active time | +5% enemy durability per five active minutes |
| Level / match completion | +8% per completed campaign level or Arena match |
| Performance: On win | +4% per encounter victory |
| Performance: On loss | +5% per complete human-party wipe |
| Performance: Forced-50 | +8% per victory; each wipe eases that encounter's retries by 2.5%, up to 25% |

A loss means every human on the reference side is down. Friendly bots do not
prevent it, and individual human knockouts do not count as separate losses.
An attempt is rearmed after the human party recovers. Repeated dead frames and
knockout-to-death transitions cannot count another wipe.

**Forced-50** replaces the old adaptive option. Its retry relief belongs only
to that unfinished encounter, applies to its subsequent attempts, and clears
when the encounter is won. It can make an initial encounter easier even before
any run pressure has accumulated. The name describes the feedback option;
its asymmetric adjustments do not guarantee a measured 50% victory rate.

General run pressure survives retries and level transitions, and caps at +150%.
It primarily changes enemy durability, with damage growing at 15% of that rate.
Physical armor and extra-enemy weapons use their own delayed equipment curves;
there is no numerical armor/passive-block multiplier. The optional AI factor is
separate. Player progression can independently grant purchased upgrades.

The original story player anchors the reference side. In other modes its
current effective team determines the relevant opponents. Campaign objectives
and Arena results provide authored encounters; modes without those events use
combat groups. Only future events use changed settings: enabling a driver does
not reprice old wins or losses, and retries cannot farm a credited victory.
Starting a fresh game from the lobby or adopting a fresh solo run resets the
ledger. Host snapshots carry the current run and retry relief to other clients.

## Host readout

**Settings → Game → Show difficulty readout** controls the larger Cella/edosz
readout in the designated host's view. It follows that player's screen
assignment and remains hidden in unscaled contexts, regardless of the checkbox.
Cinematics, loading and the host's personal menu hide it too.

The host samples enemy base difficulty, pressure, party size and scaling options on level load, checkpoint
save, retry and the start of each Arena match. During a fight, time and results
accumulate toward the next sample. Later spawns use the same sample and
existing enemies keep their injuries when the sample changes. **Applied** is
the pressure used on this attempt. **Pending** shows accumulated pressure for
the next boundary when it differs. Current encounters use installed actor modifiers and actual equipment and
reinforcements. The next forecast assumes the present party and the pending
victory result; it does not assume extra elapsed time, losses or level awards.
An unknown upcoming lineup is labeled as an estimate.

**Threat ~2.0x** estimates twice the enemy burden of that same authored encounter
at the selected preset without scaling. It multiplies the enemy-count ratio by
the average durability-times-damage multiplier. Armor coverage, weapon matchups,
AI, terrain, player skill and the additional strength of allies are outside this
estimate. The configured wave includes its defeated members; it is not a
remaining-enemy counter. Actual equipment is reported separately.

All participants need the matching prerelease and protocol. The new progression
and equipment replication are incompatible with older clients.

## Player progression

The bundled **Friendly Progression** mod supplies the XP rules, shop and loadouts. It starts enabled in **Mods**. The campaign and Arena lobby each remember their own **Progression** toggle.

Enable **Progression** under **Mode settings** for Campaign or Arena. It works
with either Traditional or Scaling difficulty. XP rewards use enemy species,
spawn equipment and the selected difficulty. Scaling also considers the enemy's
durability, outgoing damage and current run pressure.

**Campaign.** Purple XP is pending until a checkpoint or chapter victory banks
it as green XP. A reset clears the pending pool. In independent mode, living
teammates share credit for hostile kills. A downed spectator keeps the XP
already earned. The shop opens between chapters. At the last shop the host
can choose to loop, carrying purchases, balances and difficulty into the first
chapter. The host can resume a saved run through Continue or Load game.

**Arena.** Kill XP goes straight into the green balance. The optional **Combat**
rule instead awards XP for actual damage dealt and deducts XP for damage taken,
at 40% of the corresponding bounty rate. An exchange of equal damage against
equal opponents leaves a small gain. The balance has a floor of zero. The shop
opens once a team wins the match and the match-win camera finishes. Earlier rounds and draws go straight into the next round after their winner camera. The next match starts when shopping finishes.

**Independent upgrades** give each player a wallet and an individual shop on
their screen. Use the target selector for self or friendly bots. **Shared
upgrades** give each team a wallet. Its first connected human by seat ID leads
by default and can hand shopping authority to another teammate. The leader
buys stats for all human teammates. Gear and recruits bought for the team
normally rotate between eligible humans. Armor rotates through everyone on
the team, including bots. Each slot advances to its next material after every
teammate owns the current tier for that slot. Individuals keep their own
loadout choices.
Bots can receive gear and AI training. Each human marks ready, or the
multiplayer shop timer advances everyone after 10, 15, 30, 60 or 120 seconds.
Infinite waits for everyone. Solo shops always wait.

| Purchase | XP | Effect |
| --- | ---: | --- |
| Durability, damage, speed | 16, 23, 34 for the first three ranks of each stat | Each rank adds 8%, 6% or 3%, respectively |
| Passive or active stealth | Same prices as the other stats | Each rank adds 6% concealment while standing or crouching, respectively |
| Wood | 155–195 | First shoulder, forearm or thigh guard |
| Chitin | 310–600 | Second limb guard or first chest, back or head piece |
| Dog armor | 465–900 | Upgrades any Chitin slot |
| Therium grey | 775–1,500 | Upgrades any Dog armor slot |
| Dagger | 140 | Separate offhand toggle |
| Staff, sword, spear or broadsword | 360, 880, 1,040 or 1,280 | Select one large weapon or choose None |
| Recruit | 2,400 for the team's first | Starts at Casual AI. Subsequent recruits cost 2.25× the preceding price |
| Train a bot | 1,800, 3,400, then 8,600 | Hardcore, Expert, then Nightmare. Nightmare earns XP and buys its lowest stat ranks at each shop |

Weapon unlocks belong to the recipient for the session. Their selected loadout
returns on the next deployment, including checkpoint retries. Weapons that
support a sheath prefer the hip. A long weapon with no sheath uses a free hand.
An equipped large weapon replaces the campaign or Arena's supplied large
weapon. An equipped dagger adds to one supplied dagger when no large weapon
is selected. Selecting both a large weapon and a dagger gives exactly that pair.

The stats column shows each player’s armor. The green bar shows banked XP as a share of the projected final balance, which includes the estimated reward from remaining enemies. The purple bar shows pending XP against the estimated reward for the current encounter. Both bars display the actual balance for the player or shared team.

All five stat cards keep their neutral Common appearance at every rank.
Detection range is divided by `1 + 0.06 × stealth rank`, using active stealth
while crouched and passive stealth otherwise. During an unarmed or staff rear
choke, the holder’s attack damage increases knockout speed and the defender’s
durability extends the time they can resist.
