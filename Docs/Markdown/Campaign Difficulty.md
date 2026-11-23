# Campaign party difficulty

The difficulty screen and Game settings have a **Scale campaign encounters for
party size** checkbox, enabled by default. It controls enemy durability, damage,
passive-block armor, and limited reinforcements. **Casual**, **Hardcore**, and
**Expert** retain their chosen AI behavior. **Nightmare** uses Expert's base
settings and adds bounded AI pressure as the human party grows, independently
of the stat/number checkbox. One human keeps the original encounter values.

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
- Count human party members with an admitted character, including knocked-out
  humans and temporarily disconnected controllers. A defeated player does not
  make the remaining enemies weaker. Removing a party seat changes the count.
  Allied lobby bots do not increase the scaling count.
- Scale smoothly through four humans, then use diminishing growth. All stat
  increases and reinforcement counts have hard limits.

Normalized health is left intact. Durability changes incoming damage, so story
health thresholds, regeneration, and network health bars remain valid. Armor
strengthens passive blocking; it does not enable the game's metal-armor weapon
immunities or refill knockout shields. Changes never heal existing injuries.

Examples at four humans, relative to each actor's authored base:

| Opponent | Effective durability | Damage | Passive-block armor |
| --- | --- | --- | --- |
| Ordinary rat | 1.54× | 1.225× | 1.21× |
| Ordinary rabbit | 1.645× | 1.225× | 1.21× |
| Ordinary cat/dog | 1.75× | 1.225× | 1.21× |
| Cat/dog boss | 3.43× | 1.36× | 1.45× |
| Wolf boss | 3.91× | 1.36× | 1.45× |

Regular durability caps at 3× and boss durability at 6×. Damage caps at 1.6×
and 1.9× respectively; passive-block armor caps at 1.5× and 1.9×.

Nightmare improves authored aggression, blocking, follow-ups, ground aggression,
throw counters, weapon catching, and attack speed within bounded ranges. It
does not push the global difficulty slider beyond its supported range, change
game speed beyond Expert, or change authored movement speed.

## Reinforcement and story safeguards

Ordinary fights add at most **four** enemies; boss encounters at most **two**
bodyguards. Large dogs and wolves receive additions later: a solo wolf boss
does not gain another wolf until eight humans. Reinforcements use existing
encounter species, with the weakest existing member preferred for bodyguards.

There are at most **eight additions per chapter attempt**, and additions stop
when **32 enabled, awake hostile NPCs** are already present. Existing human and
allied characters are outside that hostile-NPC cap. Maps above the cap still
receive stat scaling. A killed reinforcement is never replaced during the
encounter, including after another player joins.

Adds require an active authored defeat objective, a nearby living human, safe
ground and space, and an eligible awake actor outside dialogue/static staging.
Campaign cinematics do not spawn reinforcements. The objective waits for its
reinforcements to be defeated before proceeding; authored objective strings and
editor connections remain unchanged.

Stats react to party/settings changes during play. Existing reinforcements
remain in their current fight when the party shrinks or the checkbox is turned
off. A retry removes generated actors and their unused gear and rebuilds that
attempt's budget; player-held loot is preserved. Chapter transitions start a
fresh budget. New actors and scaling factors are reliably replicated, including
for late joiners and temporary NPC ownership transfers during throws/grabs.

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
```

The native unit suite also checks solo neutrality, exact preset identity,
boss emphasis, species-aware addition counts, diminishing growth, hard caps,
and the independence of Nightmare AI from stat/number scaling.
