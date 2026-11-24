# Mode challenge settings

Select a mode in the lobby to populate **Mode settings**. The **Enemy difficulty
increases** section makes opponents tougher over time or after the listed events. The host chooses these
rules; other clients see the same values. Each mode remembers its choices during
the application's session. Arena options and campaign party scaling live here;
Hosting options contains session permissions and Start/Leave. Hover a lobby
control or focus it with a controller to read its description.

All progression options start **Off**. Each driver offers Gentle, Standard and
Brisk, worth half, normal and one-and-a-half the listed Standard rate:

| Driver | Standard change | Available in |
| --- | --- | --- |
| Active time | +5% durability per five active minutes, continuously | Every play mode |
| Level / match completion | +8% per completed chapter or Arena match | Campaign and Arena |
| Encounter win | +4% per reference-side encounter victory | Every play mode |
| Player defeats | +5% per human teammate down or death | Every play mode |

The original story player anchors the reference side. In Arena and other modes,
its current effective team determines which NPCs are opponents and which round
wins count. Promoting a decision host does not change this reference.

**Adaptive win difficulty** disables the separate player-defeat pressure driver. Each human defeat
in that encounter reduces its eventual win award by 25%, floored at zero. At the
Standard win rate, zero, one, two, three, and four deaths give +4%, +3%, +2%, +1%,
and +0%. Failed attempts at the same encounter retain their deaths. A knockout
followed by death counts once; recovery and a later defeat count again. Bot
losses do not advance the player-defeat driver. Turning encounter wins Off also
turns adaptive wins Off.

All enabled drivers strengthen enemy NPCs; player defeats never buff players.
Adaptive wins reduce only the pending victory award, never already accumulated
pressure. Awards are counted once and do not change retroactively. Completed encounters
cannot farm repeated awards by retrying. Time excludes loading, shared pauses,
editor/build sessions, ready screens and cinematics; opening a private menu
while the world continues playing still advances it.

Progression primarily increases hostile NPC durability, with damage and passive
armor receiving 15% and 10% as much growth. It adds no AI bonuses or human stat
advantages. Friendly NPCs, human players and allied lobby bots retain their own stats.
Enemy lobby bots receive progression stats; their party-size and AI bonuses
remain disabled. Arena's mode-created NPC opponents can also scale. A pure human-versus-human match
has no NPC stats for these rules to change.

Challenge pressure caps at +150% durability, with combined party/progression
multipliers capped at 6× durability, 2× damage and 2× passive armor. Existing
injuries and knockout shields are preserved. Enemy additions remain the capped
campaign party-size system; these progression drivers do not flood the map with
new actors or replace Arena's population rules.

The main campaign uses authored defeat objectives; Arena uses round results and
completed matches. Hosting a campaign map as sandbox uses sandbox combat groups
instead of importing the map's campaign-wide objective. Challenge campaigns use their combat/objective outcomes.
Other modes detect a combat group and count victory when its opponents are down
and a human on the reference side remains up. This fallback has less authored
encounter structure than the campaign and Arena integrations.

Progress and adaptive deaths survive retries and chapter transitions. Starting
a new game from the lobby resets the run; mode preferences remain. Host snapshots
carry the settings and progress to late clients and through a lobby handoff.
Every participant needs the same updated release and native protocol.
