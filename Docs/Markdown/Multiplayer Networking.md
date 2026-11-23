# Multiplayer networking

This describes the ownership-based renderer and simulation in this fork. The
game does not run a deterministic simulation on every machine.

## Authority and presentation

- A client simulates each of its local human characters. The host simulates
  AI, the world, and loose thrown weapons, and relays remote characters.
- Characters stream compact root, bone, and gameplay snapshots at up to
  30 Hz. Receivers interpolate a delayed visual pose, with bounded
  extrapolation when a new snapshot is late. Still characters send a
  heartbeat instead of an identical pose every tick.
- The defender's machine resolves a melee hit against its own gameplay state
  and replies to the attacker's machine. The attacker predicts a response
  while waiting. Throws, chokes, and drags can temporarily transfer character
  simulation ownership. This is distributed authority, rather than a fully
  authoritative server with input replay and rollback.
- Each network connection includes all its local players' camera interests.
  A fight beside a client's second player matters as much as one beside its
  first player. Logical lobby player IDs and network peer IDs are distinct.

## Traffic policy

The GameNetworkingSockets transport uses three lanes with equal scheduling
priority and weights 64:32:1. Weights divide bandwidth when lanes compete;
an idle lane does not reserve unused bandwidth.

| Lane | Messages | Delivery |
| --- | --- | --- |
| Control | Creation/removal, inventory attachment, combat decisions, ownership, session and script events | Existing reliable ordering retained |
| State | Character poses, item transforms, camera and morph updates | Replaceable state is unreliable; environment edits retain reliable delivery |
| Cosmetic | One-shot character/material/weapon sounds | Unreliable; a lost sound is preferable to delayed combat |

Looping sound lifecycle messages remain reliable. Dependent create, attach,
ownership, and removal events share the control lane to preserve their order.
Combat and ownership requests also bypass the normal small-packet batching
delay. Lanes prevent unrelated reliable traffic from imposing the same
ordered-delivery wait on control, but they cannot create bandwidth or CPU time.

The host's character stream has a 150,000-byte/second application budget,
shared across receivers. Allocation reserves update rates for the receiver's
active fights and nearby players before spending the remainder on other
characters. If even the reservations exceed the budget, they must also slow
down. This is a target for character payloads, not a cap on total wire traffic:
packet overhead, vital-state refreshes, control, and assets cost more.

Loose weapons send transforms when they change, plus a one-second healing
heartbeat. Their physics and reliable hit/attachment messages continue to run
normally. Resting weapons no longer generate reliable transforms every tick.

Melee reach checks use a bounded history of received owner root poses at the
attack timestamp, rather than the interpolation-delayed visual pose. Existing
reach limits remain. Requests more than 100 ms before the retained pose
history, or more than 1.5 seconds behind its newest pose, cannot apply a late
hit. Hit replies
echo their request timestamp: duplicates, superseded requests for the same
attacker/target/event, and replies older than 1.5 seconds are discarded. This
limits delayed corrections from a backlog. It is not full historical
defender/collision rollback.

All peers need the same release and native protocol. Protocol 8 introduced
the traffic lanes and correlated hit replies. Protocol 9 adds host-authoritative
campaign scaling factors and shared match difficulty settings; older binaries
are incompatible. Protocol 10 adds campaign faction/adversary behavior and the
higher campaign durability limits. Protocol 11 adds per-mode challenge settings
and authoritative progression snapshots; use matching updated binaries and scripts.

## Campaign and inventory consistency

Campaign story progress is shared, but progress-dependent fall-death volumes
are gated by each human's own checkpoint history. A teammate climbing ahead
must not arm a floor around a player climbing below. Native safe-placement
queries use that same history, and explicit respawn placement adopts the
anchor teammate's progress. Unconditional lava, spikes, and kill volumes
remain lethal. Host seeds establish the baseline for late joiners.

Runtime snapshot attachments can exist without editor connections. Reliable
weapon grants retain reset metadata so loadouts survive a retry. Reconciliation
restores script inventory slots after attachment invalidation;
detach removes physical attachments even without an editor connection. The
campaign inventory helper reuses a player's tracked held copy instead of
creating another because a slot is temporarily empty.

## Remaining scaling limits and useful next work

1. Snapshot capture and compression still cost CPU per simulated character;
   lowering transmission frequency does not eliminate bone capture work.
   Profile crowded scenes before changing physics or AI update rates.
2. Serialization, compression, and persistent late-join replay still run under
   shared network locks. A future bounded replay queue and shorter lock scopes
   need level-generation safeguards so old replay cannot leak into a new map.
3. The fixed aggregate snapshot budget becomes restrictive with many clients
   and actors. Measured per-connection budgets and stronger relevance filtering
   would scale better than increasing every actor's send rate.
4. Defender-owned combat remains sensitive to latency and asymmetric views.
   Full input prediction/reconciliation, historical collision validation, or
   central server authority would be substantial architectural work.
5. Relaying through a listen host makes its CPU and upload capacity a shared
   limit. These changes do not make very large battles free to synchronize.

The test bridge exposes item updates sent/skipped, stale hit replies, hit
requests/resolutions, snapshot traffic, and connection diagnostics. Use those
alongside frame times and the periodic `Link peer` transport logs (queue time
refers to the control lane; pending byte totals include all lanes) to distinguish
render/AI saturation, missing snapshots, packet loss, and reliable queue buildup.
The bridge is opt-in for isolated automated testing.

Targeted regression scenarios (after building, with the normal game closed):

```sh
python3 -I Tools/ogmp.py run gui_navigation
python3 -I Tools/ogmp.py run campaign_hazards
python3 -I Tools/ogmp.py run coop_inventory
python3 -I Tools/ogmp.py run crowded_actions
```

These create isolated test profiles and processes; they cover actual controller
navigation, editing drafts, separated campaign players and late joins, repeated
equipped retries, and secondary-player controls/combat under delayed traffic.
