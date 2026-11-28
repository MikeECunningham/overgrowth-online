# Overgrowth Online — local multiplayer fork

This is [MikeECunningham's fork](https://github.com/MikeECunningham/overgrowth-online)
of [crxhvrd/overgrowth-online](https://github.com/crxhvrd/overgrowth-online), an
unofficial continuation of [Overgrowth] built on Wolfire's open-source engine.
It combines the upstream online multiplayer and renderer with native Linux
support, local multiplayer lobbies, and multiple local players per online client.

Current version: **0.42.0-alpha.16** ([VERSION](VERSION)). This is an optimized
testing prerelease. Native Linux standalone networking has been exercised;
the new changes still need Windows and Steam-enabled release validation.

The repository contains source code and data overrides. The commercial game
assets (art, levels, sounds) come from each player's own copy of Overgrowth,
which is available from [Wolfire Games].

Overgrowth Online is not affiliated with or endorsed by Wolfire Games.

Start with [FRIENDS.md](FRIENDS.md) to clone, build and play on Linux or Windows.
[COMPILING.md](COMPILING.md) has dependency and configuration details.
[RELEASE.md](RELEASE.md) explains versioning, packaging, the release pipeline
and the remaining Steam friends/relay checks.

## Changes in this fork

- Native Linux Release builds, installed-game asset discovery, and native
  OpenGL/Vulkan shader dependencies.
- Host/connect pregame lobbies, scrolling player lists, names, teams, exclusive
  screen assignments, player removal and controller/keyboard joining.
- Multiple local players per network peer using the upstream owner simulation
  and compact pose streams; controller settings survive reconnects and rejoining.
- Independent pause/settings menus confined to each player's assigned screen,
  including simultaneous menus and private control editing.
- Transferable lobby host and local leader roles, pregame server handoff,
  host-controlled or personal team selection, and a pause menu for teams and
  live local screen assignments.
- Persistent local parties when joining an existing single-player game, with
  shared roster/device handling across campaign levels. Campaign cinematics
  fill the window and hold extra players hidden, including mid-scene joins.
- Mode-managed Arena team/FFA matchups and cooperative campaign teams, with an
  optional **Force lobby teams** override. End game returns the connected group
  to its lobby for another match.
- Campaign faction teams: opposing lobby players and bots deploy at encounters,
  with NPC faction affiliations and independent hostile wildcard teams. Friendly
  bots count toward scaling and spread their escorts across teammates.
- Three bundled gameplay mods, **Difficulty Scaling**, **Friendly Progression** and **Survival**.
  They start enabled in **Mods**. The host chooses Scaling in **Difficulty** or
  turns on **Progression** in **Mode settings** for a particular game.
  Its shop offers separate bone and flesh damage upgrades and resistance to
  long falls. In the shop, H or L2 heals and Space or R2 toggles Ready.
  Each mod can be disabled independently. Slots, split-screen, save cards
  and the armor protection models are engine features.
  See [campaign scaling](Docs/Markdown/Campaign%20Difficulty.md) and
  [XP, shops and equipment](Docs/Markdown/Mode%20Challenge.md#player-progression).
- Mods can declare per-mode lobby controls, synchronize host-owned state and
  include versioned data in save cards and Continue.
  See [session mods](Docs/Markdown/Mod%20Loading%20Design.md#session-mods).
- Font rasterization, controller focus, GUI texture loading and Linux
  compatibility fixes. The lobby uses the game's menu artwork and fonts.

The current local builds use CMake `Release` optimization, with diagnostic
symbols retained. They use standalone UDP networking; Steam friends and
relays require a separately built and tested Steamworks flavor. The inherited
packaging/CI scripts still need adaptation before publishing portable binaries.

## Rendering compared to the original game

The renderer is being rebuilt under the original look. Every step is checked against screenshots of the build before it; the look changes in the table are deliberate.

| | Original Overgrowth | Overgrowth Online |
|---|---|---|
| Graphics API | OpenGL 3.2 | OpenGL 4.6, or Vulkan 1.3 (Settings > Screen > Renderer). If Vulkan fails to start, the game goes back to OpenGL |
| Platforms | Windows, macOS, Linux | Windows, Linux (native build) |
| Graphics resources | One shared vertex array, filtering stored on each texture, hundreds of loose uniforms looked up by name | Direct state access, immutable textures, sampler objects, vertex layouts, uniform blocks with fixed bindings, per-frame data in one ring buffer |
| Shaders | GLSL compiled by the driver; a variant missed at load time compiled during play | The same GLSL, also built to SPIR-V. Vulkan supports precompiled shader caches; source builds compile and cache missing variants. Pipelines are prepared during the menus and at level load |
| Character skinning | On the CPU. The optional GPU path was slow on NVIDIA GPUs in big battles (10-12 ms a frame with 96 characters on an RTX 5070 Ti) | On the GPU, the bones read from a buffer filled each frame |
| Depth buffer | 24-bit | Reversed-Z with 32-bit float depth: more precision in the distance |
| Anti-aliasing | MSAA (2x to 8x) | MSAA, or TAA on either renderer, with a sharpness slider. Characters and items write their own motion vectors; the rest of the scene is reprojected from the camera |
| Upscaling | None | On Vulkan: NVIDIA DLSS on GeForce RTX GPUs and AMD FSR 3.1 on any GPU, at native resolution as anti-aliasing (DLAA, FSR Native AA) or in Quality, Balanced, Performance and Ultra Performance modes. They use the same motion vectors as TAA |
| Bloom | Per colour channel: sunsets and backlit ears glowed pure red | Keeps each pixel's colour |
| Noise (dithering, grain) | A sine-based hash that comes out differently on each GPU and driver | An integer hash (PCG2D): the same on every GPU, on OpenGL and Vulkan |
| Errors | OpenGL errors hidden in release builds (an unused display list built for every item) | None on the test levels, checked with an OpenGL debug context and the Vulkan validation layer |

Vulkan ray tracing is available in single-viewport play on Windows and Linux. Choose **Settings → Screen → Renderer → Vulkan**, restart the game, then choose a preset in **Settings → Ray Tracing**. Low enables sun shadows and ambient occlusion. Medium adds local-light shadows and reflections. High enables diffuse global illumination. The hardware needs Vulkan ray queries and bindless sampled images. Reflections and GI additionally require position fetch, and GI needs at least 176 bytes of push constants.

Linux supports the built-in GPU denoiser and optional NVIDIA NRD for ray-traced shadows, ambient occlusion and diffuse global illumination. When installed, NRD is selected by default and appears under **Settings → Ray Tracing → Denoiser**. **NRD lighting resolution** defaults to **Auto**, which sizes these effects using the GPU's available memory budget at setup. Select **Native** or a fixed percentage to override it. The scene and interface keep their configured resolutions. [Linux NRD build instructions](COMPILING.md#optional-nvidia-real-time-denoisers-nrd)

DLSS and FSR currently require a Windows build. Standalone Linux networking supports direct IP over UDP. Steam friends, lobby discovery, invites and Valve relays require a Steamworks build.

## Compared to Wolfire's multiplayer prototype

Wolfire's online prototype (2021) was left unfinished. Overgrowth Online started from it and reworked it:

| | Wolfire's prototype | Overgrowth Online |
|---|---|---|
| Your character | Runs on the host: every move waits for the network | Local movement runs on your machine; combat confirmations still depend on latency |
| Other characters | Full poses about 120 times a second, about 5 Mbit/s per player | Compact, interpolated streams; bandwidth and smoothness depend on scene size, latency and loss |
| Guests' frame rate | About half the host's: every streamed character's morph targets were looked up per vertex, twice a frame | Close to the host's |
| Hits | Decided on the host | Decided by the defender's own machine |
| Joining | Steam invites, or TCP by IP | Steam friends, a server browser, direct IP |
| Weapons | Pick-ups not shared | Held, thrown, stuck in a body or left on the ground: the same on every machine, tested with guests on different pings |
| Sounds | No footsteps from other players | Shared |
| Game modes | Any level, without co-op or PvP rules | Story co-op, arena PvP with bots, Sandbox Levels, Freeplay and shared editing |
| Who may change what | Any connected player could delete, spawn or move any object and rewrite player states and settings | The host checks every message: a player can only affect their own character; hits and grabs must be in reach |
| Malformed packets | Could crash the game | Dropped unread; tested with forged attacks |

See [Multiplayer networking](Docs/Markdown/Multiplayer%20Networking.md) for authority, traffic priorities, campaign consistency and current scaling limits. Native protocol **17** checks the build and the network schemas of active gameplay mods when a player joins.

## Multiplayer lobby

Open **Multiplayer** and choose **Host** or **Connect**. Hosting opens the lobby before loading a world. Guests connect by direct IP, or through the existing Steam friends and discovery features in Steamworks builds, and wait in the same lobby until the host starts.

Choose **Survival** in the lobby, or open **Play → Survival** for a local party.
It uses the Freeplay map list, including Sandbox Levels. Battery 1 has one wave,
batteries 2–3 have two, batteries 4–7 have three, and batteries 8–15 have four.
The wave count rises to five, six and seven at batteries 16, 32 and 64.
Within a battery, the HUD counts down the 20 seconds between launches even
while enemies from an earlier wave are alive. Battery and wave countdowns chirp
at three, two and one. Each lobby team faces its own enemies. A turquoise
diamond above a visible raider means it belongs to another team's wave and is
neutral to the player on that screen.

During a countdown the host prepares the upcoming formation one fighter or
weapon at a time. Each peer keeps those objects dormant until the wave starts.
When the battery-end shop opens, unattended weapons return to the last player
who carried them if that player has a compatible free hand or belt slot.

On open ground, waves spawn 12–20 units away in battery 1. Both radii grow
until the ring reaches 40–64 units in battery 20. Confined maps use a closer
ring when the preferred one has no reachable ground. Raiders approach the
deployment area and use their senses to find players. The first three
batteries spawn unarmed rats and rabbits. The wave seed determines how each team’s
budget buys a mix of fighters and weapons. Two rats cost as much as one rabbit.
Each wave has a larger budget than the preceding wave in the same battery.
The director uses each team's largest deployment that battery, so losing a
teammate cannot reduce the finale's budget. Teams with the same deployment size
receive matching formations. Eliminating
a team removes its opponents while other teams are still fighting. Clearing
a battery plays the Arena victory cue and gives the finishing hit three seconds
on camera before the shop or free upgrades open. When the last team goes down,
its opponents stay on screen for three seconds. The scoreboard shows waves
cleared and enemies defeated by each team. The host can left-click to start a
fresh run at battery 1 or right-click to return the party to the lobby.
Retry in the Survival pause menu also starts a fresh run. Continue on a game-over autosave
reopens that lobby with its settings and slots.

The wave countdown has a dark background for visibility against the sky. The
host’s difficulty readout stays visible through battery breaks, including while
players shop. Each lobby map picker starts with **Random map**, which saves the current filter. The host draws a map from that list when starting the game.

With Progression enabled, the host can check **XP handicap** on a human slot
in the Players panel. A defeat earns committed XP there. **Local leaders may
set XP handicaps** lets each computer’s leader choose for its own players.

The lobby has separate panels for the mode and map, the scrolling player list, and hosting options. **Sandbox Levels** offers Wolfire's sandbox maps when the level pack is active. **Freeplay** opens a standalone map for play, including maps from active mods, the sandbox pack, and levels saved by the host. **Build a level** opens the shared editor. The host chooses the mode, map, manual lobby teams, pause permissions and start time. **Force lobby teams** is off by default. Campaigns retain their cooperative teams, Arena chooses randomized team/FFA matchups from its battle setups, and Sandbox, Freeplay and Build use lobby teams. Checking it enforces the lobby's team dropdowns. Mode-assigned teams are synchronized separately, so random Arena matchups do not overwrite the saved lobby choices.

The device that chooses Host controls the host seat. The device that chooses Connect controls the joining computer’s leader seat. Each computer’s local leader can change **Settings → Game → Automatic Ledge Grab** for that computer. Match difficulty, saved games and host changes preserve this preference.

Press a button on another controller to add a local player. If a controller owns P1, press **Enter** to join with the keyboard and mouse as another player. Each client's **local leader** can rename or remove its local players and assign their screens; the host can also remove remote players. **Promote to leader** transfers local authority without changing device assignments. After transferring both host and leader roles away from the initial keyboard/mouse player, that seat can be removed. Removing a remote leader disconnects that client's whole group. Screens are exclusive: choosing an occupied screen swaps the two players. Remote players have no screen on your client. Each client renders only its own screens, and every player's character uses the fork's existing local simulation and compact network streams.

**Promote to host** transfers lobby and match authority. Choosing a player on another computer also transfers the pregame network session, so the original hosting computer can leave after the handoff completes. Every client first checks that it can reach the new host; a failed reachability check leaves the original lobby running. Direct-IP hosts need a reachable UDP port, while Steamworks builds can use Steam P2P. Host transfer is available in the lobby; return there before handing off a running match.

The lobby header shows who is operating the shared menu, the match host and the
local leader. Only the host and each computer’s local leader can edit the lobby. Promoting another
local player keeps this computer's host/client status; either the host or local
leader can close or leave the lobby. Other local players see which controller
to use. The button used to join a controller does not also activate a menu item.

**Host controls teams** defaults to on. When it is off, local leaders can assign teams to their couch players in modes that permit manual teams; the host keeps team-management authority. Mode-managed campaign/Arena teams remain governed by the selected mode unless overridden. In Arena, the host can change a fighter’s team for the current match. In cooperative campaigns, the party keeps its story faction until **Force lobby teams** is enabled. The pause menu's **Teams** page shows the roster and team controls. Local leaders can also swap local screen assignments there during a match.

The host's P1 starts as Turner. Other players start with their team's fur and clothing colors; the numbered team dropdowns include matching color swatches. The lobby uses the game's menu artwork and fonts, with native scrolling and controller navigation underneath.

The host can choose **Add slot** in the lobby or pause menu. A new slot starts with a bot. In the lobby, switch it to a player slot to reserve an opening. The admission dropdown controls whether remote players and local controllers can create slots or must take an opening. Swapping occupants moves control to the destination unit, including its XP, upgrades and equipment.

**Settings → Profiles** manages named players, their outfit for each race and their preferred weapon appearances. **Use for solo Play** assigns the chosen profile when starting a private game. In a lobby, each local leader can choose a host-local profile for each couch player, create one, or keep that seat as a temporary guest. L2/R2 change cosmetic categories and L1/R1 cycle available appearances. Lobby rows use team colors, with a darker shade for bots.

**Settings → Mod settings → Friendly Progression** has independent **Cosmetic progression** and **Non-cosmetic progression** checkboxes. Each profile earns one token every 10 campaign levels cleared, 10 Arena matches won or 10 Survival batteries survived. The three counters carry over independently between sessions for named profiles. Spend a token on **Roll cosmetic** or **Roll upgrade** in **Settings → Profiles** or in the player's lobby entry. Each draw gives something the profile has yet to unlock. Players share the upgrades their profiles bring to a map at its start. Turning cosmetic progression off restores the original outfit and weapon selections.

**Save game** in the pause menu offers six manual slots on the host’s machine. **Load game** is available from the main menu, lobby and pause menu. Each card shows a camera preview, mode, map and save time. **Continue** opens the autosave, updated at level entry, campaign checkpoints and Arena deployments. Loading a lobby game first restores its settings and roster in the lobby. The host can reassign the saved units before pressing **Start**. Manual campaign saves restore authored actors and objective progress. Loading an Arena fight restarts its round with the saved scores and match setup. A save made in a shop reopens that shop.

Bots appear in the
same roster, with host-managed names, teams and removal, but have no controller
or screen. They stay with the party between levels and when returning to the
lobby. In automatic Arena matches, players and lobby bots get first choice of
the two to four battle slots. Temporary opponents fill open slots for that match.
Larger parties play together. With **Force lobby teams** enabled, Arena waits
for two opposing teams from the lobby. New seats enter at the next round.
Elsewhere, bots use the existing combat
AI and idle allies follow a living teammate using the game's navigation. This
does not add new parkour or pathfinding abilities. Party bots are hidden and
held during campaign dialogue alongside extra human players.

Run `python3 Tools/ogmp.py run lobby_bots roster_bots_arena arena_autofill bot_migration` for
bot permissions, live spawning, screen/input ownership, Arena rounds and server
handoff checks.

During multiplayer, Start opens a menu inside the requesting player's screen without stopping the world. Multiple players can open their own menus and settings simultaneously; navigating or closing one does not affect the others. Their controls, camera inversion and sensitivity belong to that local player; the local leader also controls the client's shared graphics and audio settings. Open settings pages refresh when those shared values change. By default only the designated host can explicitly pause the shared world. The host can allow all players to pause. Escape or Start pauses automatically when the host is the only human, including in a lobby with bots. With multiple humans, an unbound P opens the keyboard player’s menu and pauses if that player has permission. Closing the shortcut menu releases its pause. A separately requested world pause stays in effect. Reconnecting a controller restores its player and settings for that client session. New controllers can also join during play through the same roster used by the lobby.

Adding a player to an existing single-player level creates a local party without
restarting the world or opening an online listener. Names, teams, device profiles,
roles and screen assignments survive subsequent campaign levels. The host can
explicitly choose **Open to online players** from the pause menu or party lobby
to accept remote connections.

Campaign cutscenes and dialogue use one full-window view. The authored main
character keeps its scripted choreography; extra human characters and their
weapons are hidden and held still. This also applies to players joining during
the scene. When it ends, split-screen assignments return and extra players are
placed safely near the main character with their prior health/control state.

In a campaign party, losing every human on the main character's team restarts
the last checkpoint after five seconds; surviving bots do not keep it running.
While spectating, **Q/E** or controller **L1/R1** cycles the watched character.
Once every human teammate is down, any downed human can press attack (**LMB**
on keyboard/mouse) to restart the checkpoint immediately, including remote
players. Otherwise the five-second automatic restart remains active.
Friendly roster members, including living bots, regroup at the restored
checkpoint. A solo player retains the usual manual retry controls.

The designated host can choose **End game** from the pause menu to bring everyone back to the same lobby. Connections, players, names and screen assignments stay in place for the next match. **Main menu** still leaves the multiplayer session.

All clients need this build: the lobby and multiple-player protocol deliberately rejects older builds. Run `python3 Tools/ogmp.py run local_lobby lobby_lifecycle player_menus mode_teams` for the input, lobby lifecycle, independent menus and mode-owned team regressions. Set `OGMP_KEYS=renderer=vulkan` to check the Vulkan backend.

Additional regressions cover host/leader transfer and live screens
(`lobby_authority`), failed-handoff recovery (`lobby_rollback`), solo Stucco Courtyard (`stucco_solo`), repeated Rock Cave
matches (`cave_spawns`), and loose weapon playback when the host's camera looks
away (`loose_weapon_playback`). Run them with `python3 Tools/ogmp.py run` followed
by the scenario names. The connection dialog remains cancellable while waiting
for the version/mod handshake to finish.

`lobby_role_ui` checks promotions and exit through the rendered lobby using
keyboard and virtual controller input. `connection_cancel` checks a departed
host, repeated connection cancellation, timeout recovery and reconnecting.

Campaign regressions cover joining an offline story with a controller, real
chapter progression, and a remote party joining during authored dialogue
(`newcampaign_local`). Separate checks move host/leader authority without
changing the story actor (`newcampaign_roles`) and remove the original player
while retaining control of that actor (`newcampaign_main_removal`). These use
process-local virtual controllers through the opt-in test bridge. Retry during
dialogue is checked separately with `newcampaign_cinematic_retry`.

## Local split screen

Press a button on an unassigned controller to join the local party and enable split screen. If a controller owns P1, press **Enter** to join with keyboard and mouse. With two players, screen 1 spans the top half and screen 2 spans the bottom half. Larger parties fill the grid from the top left. Three or four players use 2×2, five or six use 3×2, and seven through nine use 3×3. Unused cells are black. Player, input and camera lists grow dynamically rather than stopping at four players.

Keyboard and mouse control P1 initially; controllers join or reconnect through the same local roster. Added players spawn on checked ground near the group. Their runtime characters are excluded from level saves and are recreated for their retained roster entries when a level reloads.

Run `python3 Tools/ogmp.py run local_splitscreen` to check nine screens, independent P5 input and party persistence through a level reload. Set `OGMP_KEYS=renderer=vulkan` to check the Vulkan backend.

## Build a level

Several people can build one level together, live. Choose Multiplayer > Host, then the **Build a level** mode card in the lobby. It offers an empty level, your saved levels and autosaves, mods' levels and the campaigns' levels. The level you pick is hosted as a build session and opens in the editor when the host starts. Friends connect through the lobby. Each player who is allowed to build opens their own editor with F1 and goes back to play with F1. The game keeps running for everyone meanwhile. A line at the top of the screen says what F1 and Ctrl+S do and whether time is running.

The host controls the session from the Build session panel in the editor (top right), or from Windows > Multiplayer settings:

- **Freeze time** pauses the game for everyone.
- **Restart the level** restarts it for everyone, after rebuilding the AI's paths.
- **Rebuild the AI's paths** appears once edits have made them out of date.
- **Players who join may build** and a box per player decide who may use the editor.
- **Builders may save** lets builders save with Ctrl+S.
- **Edit lines in the chat** posts lines like "Bob placed 3 objects and changed 1 object".

In any other online session, the host can turn building on with "Building in this session".

### What you can do

- **Edit with every editor tool:** place, move, rotate and scale objects, copy and paste, delete, group and ungroup, use prefabs, change object properties, colours, script parameters and connections, decals, lights and hotspots, including modded objects. Anything an object saves in its parameters goes to everyone.
- **Change the sky and the level's settings.** These are sent after an editor operation, or at once while time is frozen.
- **See the other builders:** each one's camera appears as a named marker in their own colour, and what they have selected is outlined.
- **Watch drags live:** other builders see your objects move while you drag them, and the final position arrives when you let go.
- **Undo and redo** (Ctrl+Z, Ctrl+Y) your own edits. Everyone has their own history.
- **Join a session already under way:** you load the level, then receive every edit made since it was loaded or last saved.
- **Rely on autosaves:** while a build session is on, the host saves a copy every 5 minutes if anything changed. It goes to `Data/Levels/autosave/<level>_autosave.xml` in the host's write folder, and the level's own file is left alone. The editor shows when the level was last saved and whether there are unsaved changes.
- **Get fresh reflections and AI paths:** reflection captures re-render on every machine shortly after edits stop. The host rebuilds the AI's paths when the level restarts and saves them with the level.

### What you can't do (yet)

- **Edit an object someone else has selected.** The first player to select an object holds it until they deselect it. Others' changes to it are put back, with a message saying why.
- **Move players' characters.** A player's character isn't part of the level, so a builder can't move someone else's character. Moving your own isn't saved into the level either.
- **Save unless allowed.** Only the host's copy of the level is saved. Without "Builders may save", a builder's Ctrl+S asks the host, who gets a Save / Not now prompt.
- **Use files only you have.** Builders can only place what every machine has: the stock game and the mods everyone has enabled. New models, textures or sounds aren't sent. The one exception is the level file itself: a level from the host's write folder is sent to the guests.
- **Edit things that move by themselves without selecting them.** While time runs, characters, items and hotspots count as edits only while you hold them (selected) and use an editor tool on them. Otherwise their movement is the game's, and nothing is sent. Freeze time to rearrange a level that is in motion.
- **Keep a hotspot's running state.** An edited object is recreated from its saved description on every machine, so a hotspot's script state that isn't stored in its parameters restarts. Data a mod keeps outside the level, such as in the save file, isn't synced. A mod can sync that data itself through the [script API](Docs/Markdown/Overgrowth%20Online%20Script%20API.md): `Online_Send`, `Online_SendToHost` or `Online_SendToPlayer` to send, `Online_Listen` to receive, and the `online:joined` event to bring new players up to date.
- **Save the AI's paths under a folder with non-English characters.** This is a known issue from the original game: a level saved inside the game's folders under such a folder name fails when its AI paths are written.

## Mods

Other mods are not guaranteed to work online and have not been tested.

- The Multiplayer menu opens only when every enabled mod declares online support (`<SupportsOnline>true</SupportsOnline>` in its `mod.xml`). Most Workshop mods don't, so they have to be disabled to play online. The declaration is the mod author's own claim; nothing checks it.
- Everyone in a game needs the same mods enabled. Only the mod names are compared, not their versions: different versions of the same mod can still join each other and fall out of sync.
- Mods that only add assets (characters, weapons, maps, textures) are the most likely to work. Mods that replace the character scripts (`aschar.as`, `enemycontrol.as`) break online play: the network code relies on hooks in those scripts.
- Source builds list the optional Overhaul mod (AI, HUD, camera, weather, trainer, Large Battles) in **Main menu → Mods**. All players in a multiplayer session should use the same activation choice. [Overhaul AI tuning guide](Docs/Markdown/Overhaul%20AI.md)

## License

Based on [Overgrowth's source code](https://github.com/WolfireGames/overgrowth). Unless otherwise noted, the code is licensed under `Apache-2.0`; see [LICENSE]. Third-party libraries keep their own licenses.

[Overgrowth]: https://overgrowth.wolfire.com
[Wolfire Games]: https://wolfire.com
[LICENSE]: LICENSE
