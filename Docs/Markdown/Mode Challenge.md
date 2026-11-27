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
| Level / match completion | +8% per completed campaign level, Arena match or Survival battery |
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
and Arena results provide authored encounters. Survival counts a wave victory
once every surviving team has cleared that wave, and reports a wipe when the
last team falls. Modes without those events use combat groups. Only future events use changed settings: enabling a driver does
not reprice old wins or losses, and retries cannot farm a credited victory.
Starting a fresh game from the lobby or adopting a fresh solo run resets the
ledger. Host snapshots carry the current run and retry relief to other clients.

## Host readout

**Settings → Game → Show difficulty readout** controls the larger Cella/edosz
readout in the designated host's view. It follows that player's screen
assignment and remains hidden in unscaled contexts, regardless of the checkbox.
Cinematics, loading and the host's personal menu hide it too.

The host samples enemy base difficulty, pressure, party size and scaling options on level load, checkpoint
save, retry and the start of each Arena match. Survival samples new waves and uses
the size of the team each enemy faces. Enemies from earlier waves keep their stats.
During a fight, time and results
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

The bundled **Friendly Progression** mod supplies the XP rules, shop and loadouts. It starts enabled in **Mods**. Each supported mode remembers its own **Progression** toggle.

Enable **Progression** under **Mode settings** for Campaign, Arena or Survival. It works
with either Traditional or Scaling difficulty. XP rewards use enemy species,
spawn equipment and the selected difficulty. Scaling also considers the enemy's
durability, outgoing damage and current run pressure.

**XP shop** and **Free upgrade choice** are independent. The shop starts on
and lets players spend committed XP between campaign levels or Arena matches.
With **Free upgrade choice** enabled, each new campaign checkpoint or completed
Arena round rolls separately for each participating human player. A successful
roll offers up to four eligible shop upgrades. Pick one for free with **1–4**
or the matching numpad keys. On a controller, D-pad up, right, down and left
choose cards 1, 2, 3 and 4 respectively. The other players can keep playing. Offers wait
for their owner and saves retain outstanding choices with their slots.

The shop and stats column show **Luck** only while **Free upgrade choice** is enabled.
Existing Luck ranks carry over when the host turns the option back on.
Luck starts at a 20% chance per roll. The first rank raises it to 24%. Each
new rank adds a smaller bonus until rank 20, which adds one percentage point
and brings the chance to 70%. Later ranks add one point each, reaching 100%
at rank 50. Luck also increases the chance of drawing expensive upgrades.
A player can improve both odds by buying Luck in the shop or taking
it as a free upgrade. In shared mode the team shares its Luck rank, and each
player still gets a separate roll.

These modes have independent checkboxes for damage dealt, kills, damage taken
and defeat. Kill rewards start enabled. With damage rewards enabled, dealing a
full health bar of damage earns the opponent's bounty. With both rewards
enabled, the killing blow can earn damage XP and a kill bounty. The damage
penalty costs 40% of a bounty per full health bar lost and draws from pooled
XP down to zero. Defeat costs one bounty. It uses pooled XP first and takes
any remainder from committed XP. For a 16 XP penalty with 10 XP pooled,
the committed balance loses 6 XP.

With progression enabled, the host can select **XP handicap** on each human
slot in the Players panel. A defeat awards that slot one defeat bounty in
committed XP, including when **Lose XP on defeat** is enabled. Shared
upgrades credit the team's wallet. The slot keeps its handicap when saved or
reassigned. **Local leaders may set XP handicaps** lets each computer's
designated leader edit its own players. This permission starts off.

**Campaign.** Purple XP is pending until a checkpoint or chapter victory banks
it as green XP. A new checkpoint can also award a 25 XP bonus. A retry clears
the pool left after defeat penalties. In independent mode, living teammates
share credit for hostile kills. A downed spectator keeps their remaining XP
while teammates continue. The paid shop opens between chapters when enabled. At campaign end the
host can choose to loop, carrying upgrades, balances and difficulty into the first
chapter. The host can resume a saved run through Continue or Load game.

**Arena.** Combat XP goes straight into the green balance by default. When
**Lose pooled XP on damage** or **Lose XP on defeat** is enabled, combat XP goes into the purple pool and
the balance left at the end of each round banks automatically. The host can enable a
25 XP round-win bonus and a 100 XP match-win bonus independently. Winning the
deciding round with both enabled earns 125 XP. The paid shop opens once
the match-win camera finishes. Players can carry pending free choices into the next round or match.

**Survival.** A team banks its pooled XP when it clears its own enemies from a
wave. The round-win option awards a wave bonus, and the match-win option awards
a battery bonus to surviving teams. Free upgrades roll after the finishing
camera at the start of the battery break. A full wipe opens the team scoreboard.
The host can left-click to retry or right-click to return the party to the lobby.
In Survival, a finite upgrade timer adds its seconds to the next battery
countdown. Zero keeps the ordinary break. Players can shop until the first wave
deploys, or mark themselves ready earlier. Infinite waits for players to finish.

**Cosmetic progression** and **Non-cosmetic progression** are independent
checkboxes in Settings → Mod settings → Friendly Progression. When either is enabled,
a profile earns one token for every 10 campaign levels cleared, 10 Arena matches
won or 10 Survival batteries survived. Each mode tracks its own milestones.
Named profiles retain unfinished milestones across sessions, and loading an
earlier save preserves credit for clears already counted. Both roll buttons
appear in **Settings → Profiles** for the selected profile and in each player's
lobby entry when their corresponding options are enabled. **Roll cosmetic** and
**Roll upgrade** each spend one token from the same balance and draw from that
profile's remaining prizes.

Profiles start with zero gameplay upgrades. On each fresh map the host combines
the upgrades of participating humans across teams. Anyone in that lobby can use
the combined pool for the map, including bots and late joiners. Unlocks first
affect the shop and free upgrade choices at the next map start. Saved maps retain
their original pool. Guests keep their progress and unlocks for the current
session.

Each basic stat unlock covers all purchasable ranks of that stat. Armor unlocks
one material for one body slot. Race and companion AI unlock one step at a time.
An upgrade roll first draws among rarity tiers that contain eligible, unowned
upgrades, with relative weights of 65,536, 512, 64, 8 and 1 from common through
legendary. It then chooses uniformly within that tier. Only the eight basic stats
are common. With every tier available, the chance to draw a remaining stat is
about 99.1%. Higher armor, race and training tiers require their previous unlock.

Cosmetic rolls use per-item weights of 32 for common, 16 for uncommon, 8 for
rare, 3 for epic and 1 for legendary. Turning cosmetic progression off restores
the profile's original outfits and weapon selections. Turning it on again
restores the saved cosmetic collection.

**Independent upgrades** give each player a wallet and an individual shop on
their screen. Use the target selector for self or friendly bots. **Shared
upgrades** give each team a wallet. Its first connected human by seat ID leads
by default and can hand shopping authority to another teammate. The leader
buys stats for all human teammates. Gear and recruits bought for the team
normally rotate between eligible humans. Armor rotates through everyone on
the team, including bots. Each slot advances to its next material after every
teammate owns the current tier for that slot. Individuals keep their own
loadout choices.
Bots can receive gear and AI training. The multiplayer upgrade timer can
advance everyone after 10, 15, 30, 60 or 120 seconds.
Infinite waits for everyone. Solo Campaign and Arena shops wait for the player.

In the shop, **Space** buys the displayed heal for the selected target and
**Enter** or **keypad Enter** toggles Ready for the keyboard player. **E** or
the controller’s confirm button activates the focused control. Stats, armor,
daggers, main-hand weapons and character upgrades each start their own row.
Race upgrades and recruitment are in the Characters section.

Each stat card shows the gain for its next rank. The gain decreases linearly
from the first rank through rank 20. Every later rank gives the same gain as
rank 20, while purchase prices continue to rise.

| Purchase | XP | Effect |
| --- | ---: | --- |
| Durability, bone damage, flesh damage, speed | 16, 23, 34 for the first three ranks of each stat | Durability starts at +16%, either damage stat at +12%, and speed at +6%. From rank 20 the respective gains are +4%, +3% and +1.5%. |
| Fall resistance | Same prices as the other stats | The first rank reduces damage from a high-speed landing by about 10.7%. It also protects ragdolls during long descents. The character's authored fall multiplier still applies, and level death volumes enforce their map boundaries. |
| Passive or active stealth | Same prices as the other stats | Sight and hearing ranges shrink by 7% of their original range at rank 1 and by 0.25% per rank from rank 20 onward. |
| Luck | Same prices as the other stats | Increases the chance of a free choice and improves the odds of drawing higher-priced upgrades |
| Restore health | Up to 8 + 2 per completed campaign level, Arena round or Survival wave | The card shows current price / maximum XP. The most depleted of permanent, temporary, blood and block health determines the price, rounded to the nearest XP. Any injury costs at least 1 XP. A healthy fighter shows 0 XP. If your balance is smaller than the quote, you can spend it all to repair that fraction of each health pool and reduce ongoing bleeding. A free choice heals in full. |
| Race upgrade | 1,200 / 2,400 / 9,600 / 38,400 | Mouse → Rabbit → Cat → Dog → Wolf. Shared upgrades rotate among humans at the lowest race. Select a companion to upgrade it individually. Companions start as rabbits. |
| Wood | 155–195 | First shoulder, forearm or thigh guard |
| Chitin | 310–600 | Second limb guard or first chest, back or head piece |
| Dog armor | 465–900 | Upgrades any Chitin slot |
| Therium grey | 775–1,500 | Upgrades any Dog armor slot |
| Daggers and throwing knives | 350 | Separate offhand toggle |
| Sticks and scabbards, tools and torches, staves, machetes and hatchets | 450, 475, 500, 600 | Select one main-hand category |
| Spears and glaives, swords, hammers, broadswords | 1,000, 1,200, 1,320, 1,480 | Select one main-hand category |
| Recruit | 2,400 for the team's first | Starts at Casual AI. Subsequent recruits cost 2.25× the preceding price |
| Train a bot | 1,800, 3,400, then 8,600 | Hardcore, Expert, then Nightmare. Nightmare earns XP and buys its lowest stat ranks at each shop |

Equipment lists each race the selected fighter can wear, from their starting race
through their highest purchase. Selecting a race is free. A new purchase equips
the new race. Shared purchases go to the next human with the lowest *purchased*
tier, even when a teammate chooses to wear a lower tier. Companions keep a
separate selection. Save cards preserve the owned range and equipped race for
each unit.

Weapon unlocks belong to the recipient for the session. Their selected loadout
returns on the next deployment, including checkpoint retries. Weapons that
support a sheath prefer the hip. A long weapon with no sheath uses a free hand.
An equipped large weapon replaces the campaign or Arena's supplied large
weapon. An equipped dagger adds to one supplied dagger when no large weapon
is selected. Selecting both a large weapon and a dagger gives exactly that pair.

The stats column shows each player’s armor. The green bar shows banked XP as a share of the projected final balance, which includes the estimated reward from remaining enemies. The purple bar shows pending XP against the estimated reward for the current encounter. Both bars display the actual balance for the player or shared team.

All eight stat cards keep their neutral Common appearance at every rank.
Active stealth reduces detection range while crouched and passive stealth
applies otherwise. Bone damage increases blunt impacts and the knockout speed
of unarmed or staff rear chokes. Flesh damage increases cuts and the bleeding
they cause. An attack that does both applies each multiplier to its own portion.
The defender's durability also extends the time they can resist a choke.

### Weapon categories and skins

A shop unlock buys a gameplay category. Its new weapon uses the recipient's
selected skin. Bots receive the mundane skin. A dropped, stolen, recovered or
sold item retains its appearance. Its resale price comes from the category.
Existing saves merge variant purchases and pending reward cards into the same
categories. Painted flint knives share the plain flint knife's category.
The Cinderbreathe knife, spear, sword, broadsword and sword scabbard share
their dog equivalents. The two glaives retain separate entries because their
reach differs. The glass and metal macuahuitls have different collision meshes.

An active mod can put a `weapon_categories.xml` file beside its `mod.xml`.
Use a stable mod-prefixed category ID and a stable skin ID within each category.
The first skin is the mundane item. A category without alternatives still has
a shop card. Use `Extend` to add a skin to an existing category. For example,
a mod with ID `my-arsenal` could declare the following items, using its own
item XML and thumbnail paths.

```xml
<WeaponCategories>
  <Category id="my-arsenal.sabre" label="Sabre" hand="main" price="sword" rarity="3">
    <Skin id="plain" label="Plain sabre" path="Data/Items/MyArsenal/sabre.xml"
          artwork="Data/UI/MyArsenal/sabre.png" rarity="0"/>
    <Skin id="royal" label="Royal sabre" path="Data/Items/MyArsenal/royal_sabre.xml"
          artwork="Data/UI/MyArsenal/royal_sabre.png" rarity="4"/>
  </Category>
  <Extend id="spear">
    <Skin id="my-arsenal.oak" label="Oak spear" path="Data/Items/MyArsenal/oak_spear.xml"
          artwork="Data/UI/MyArsenal/oak_spear.png" rarity="1"/>
  </Extend>
</WeaponCategories>
```

Choose `main` or `off` for the loadout slot. Price families are `dagger`,
`stick`, `tool`, `machete`, `staff`, `spear`, `sword`, `hammer` and
`broadsword`. Category rarity colors the shop unlock. Skin rarity ranges
from 0 (common) to 4 (legendary) and determines the cosmetic roll weight.
Each physical item path can belong to one category. Skin files share the
representative's gameplay declarations, including mass, grip, reach, line
points and materials, attack overrides, reactions and animations. Point their
appearance XMLs at the same model and sharpness map to preserve the physical
collision hull and thrown-weapon penetration. Color and normal textures may
vary. The `cubemap` shader uses a distinct tangent-enabled model representation
and must match across skins. The comparison accepts reordered XML attributes,
indentation and equivalent numeric spellings for grip hand count, reach,
reach multiplier, point coordinates and mass with the same units. It preserves
element order, unit suffixes, mesh and sharpness-map paths and unknown fields,
even when different declarations might behave alike. Bullet derives a weapon's physical hull
from its model path. The item supplies the combat-line materials. Scenery
uses its own object-level material overrides. The registry skips incomplete
items, including missing animations and appearance dependencies, and logs
skins with mismatched mechanics. Online players need the same asset mod
installed to display its physical items.
