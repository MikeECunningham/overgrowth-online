# Difficulty modes and progression

Choose **Traditional** or **Scaling** at the top of the lobby, then open
**Difficulty** to configure that mode. The same controls are under
**Settings → Game → Difficulty** during play. Traditional uses the original
Casual, Hardcore and Expert presets. Scaling keeps the selected preset as its
base and enables optional party, physical equipment, AI and progression factors.
The old Nightmare choice is replaced by the independent **AI scaling** checkbox.

The host owns shared difficulty choices. They are replicated to late joiners
and preserved through a host handoff. Fresh installations start in Traditional;
existing enabled party/Nightmare settings migrate to Scaling. Scaling also
uses the private local simulation when a solo game has no multiplayer lobby.
No network listener is opened for that private session.

## Progression rules

All optional progression drivers start Off. Gentle is half the Standard rate;
Brisk is one and a half times Standard. Time counts active gameplay and excludes
loading, ready screens, shared pauses, editor sessions and cinematics.

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
separate. Human stats and allied bots retain their base values.

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

Current encounters use installed actor modifiers and actual equipment and
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
