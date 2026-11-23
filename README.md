# Overgrowth Online — local multiplayer fork

This is [MikeECunningham's fork](https://github.com/MikeECunningham/overgrowth-online)
of [crxhvrd/overgrowth-online](https://github.com/crxhvrd/overgrowth-online), an
unofficial continuation of [Overgrowth] built on Wolfire's open-source engine.
It combines the upstream online multiplayer and renderer with native Linux
support, local multiplayer lobbies, and multiple local players per online client.

Current version: **0.42.0-alpha.5** ([VERSION](VERSION)). This is an optimized
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

Next: GPU culling with multi-draw indirect and compute particles, then Intel XeSS and ray tracing on Vulkan.

The Linux build includes the core OpenGL/Vulkan renderer and TAA. The current DLSS, FSR and NRD integrations use Windows SDK binaries and are not included on Linux. Standalone Linux networking supports direct IP over UDP; Steam friends, lobby discovery, invites and Valve relays require a Steamworks build.

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
| Game modes | Any level, without co-op or PvP rules | Story co-op, arena PvP with bots |
| Who may change what | Any connected player could delete, spawn or move any object and rewrite player states and settings | The host checks every message: a player can only affect their own character; hits and grabs must be in reach |
| Malformed packets | Could crash the game | Dropped unread; tested with forged attacks |

See [Multiplayer networking](Docs/Markdown/Multiplayer%20Networking.md) for authority, traffic priorities, campaign consistency and current scaling limits. Native protocol **8** requires matching updated binaries on every computer.

## Multiplayer lobby

Open **Multiplayer** and choose **Host** or **Connect**. Hosting opens the lobby before loading a world. Guests connect by direct IP, or through the existing Steam friends and discovery features in Steamworks builds, and wait in the same lobby until the host starts.

The lobby has separate panels for the mode and map, the scrolling player list, and hosting options. **Build a level** is a mode card alongside the playable modes. The host chooses the mode, map, manual lobby teams, pause permissions and start time. **Force lobby teams** is off by default: campaigns retain their cooperative teams, Arena chooses randomized team/FFA matchups from its battle setups, and sandbox/build uses the lobby teams as a fallback. Checking it enforces the lobby's team dropdowns. Mode-assigned teams are synchronized separately, so random Arena matchups do not overwrite the saved lobby choices.

Press a button on another controller to add a local player. If a controller owns P1, press **Enter** to join with the keyboard and mouse as another player. **Add local player (debug)** creates a seat without hardware. Each client's **local leader** can rename or remove its local players and assign their screens; the host can also remove remote players. **Promote to leader** transfers local authority without changing device assignments. After transferring both host and leader roles away from the initial keyboard/mouse player, that seat can be removed. Removing a remote leader disconnects that client's whole group. Screens are exclusive: choosing an occupied screen swaps the two players. Remote players have no screen on your client. Each client renders only its own screens, and every player's character uses the fork's existing local simulation and compact network streams.

**Promote to host** transfers lobby and match authority. Choosing a player on another computer also transfers the pregame network session, so the original hosting computer can leave after the handoff completes. Every client first checks that it can reach the new host; a failed reachability check leaves the original lobby running. Direct-IP hosts need a reachable UDP port, while Steamworks builds can use Steam P2P. Host transfer is available in the lobby; return there before handing off a running match.

The lobby header shows who is operating the shared menu, the match host and the
local leader. Each controller uses its player's permissions. Promoting another
local player keeps this computer's host/client status; either the host or local
leader can close or leave the lobby. Other local players see which controller
to use. The button used to join a controller does not also activate a menu item.

**Host controls teams** defaults to on. When it is off, players can change their own teams in modes that permit manual teams; the host keeps team-management authority. Mode-managed campaign/Arena teams remain governed by the selected mode unless overridden. Explicit host edits during play override that player's current team; future matches still follow the mode unless **Force lobby teams** is enabled. The pause menu's **Teams** page shows the roster and team controls. Local leaders can also swap local screen assignments there during a match.

The host's P1 starts as Turner. Other players start with their team's fur and clothing colors; the numbered team dropdowns include matching color swatches. The lobby uses the game's menu artwork and fonts, with native scrolling and controller navigation underneath.

The host can choose **Add bot** in the lobby or pause menu. Bots appear in the
same roster, with host-managed names, teams and removal, but have no controller
or screen. They stay with the party between levels and when returning to the
lobby. Arena uses these explicit bot entries instead of a separate autofill
setting; new bots enter the next round. Elsewhere, bots use the existing combat
AI and idle allies follow a living teammate using the game's navigation. This
does not add new parkour or pathfinding abilities. Party bots are hidden and
held during campaign dialogue alongside extra human players.

Run `python3 Tools/ogmp.py run lobby_bots roster_bots_arena bot_migration` for
bot permissions, live spawning, screen/input ownership, Arena rounds and server
handoff checks.

During multiplayer, Start opens a menu inside the requesting player's screen without stopping the world. Multiple players can open their own menus and settings simultaneously; navigating or closing one does not affect the others. Their controls, camera inversion and sensitivity belong to that local player; the local leader also controls the client's shared graphics and audio settings. Open settings pages refresh when those shared values change. By default only the designated host can explicitly pause the shared world. The host can allow all players to pause. Single-player still pauses when opening the menu. Reconnecting a controller restores its player and settings for that client session. New controllers can also join during play through the same roster used by the lobby.

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

## Local split-screen debugging

Open the pause menu as the local leader and select **Add player (debug)**. Each press adds another persistent party member and enables split screen. Players fill the grid from the top left: two use 2×1, three or four use 2×2, five or six use 3×2, and seven through nine use 3×3. Unused cells are black. Player, input and camera lists grow dynamically rather than stopping at four players.

Keyboard and mouse control P1 initially; controllers join or reconnect through the same local roster, and debug slots without a device stay idle. Added players spawn on checked ground near the group. Their runtime characters are excluded from level saves and are recreated for their retained roster entries when a level reloads.

Run `python3 Tools/ogmp.py run local_splitscreen` to check the pause button, nine screens, independent P5 input and party persistence through a level reload. Set `OGMP_KEYS=renderer=vulkan` to check the Vulkan backend.

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
- The optional, separately distributed Overhaul mod (AI, HUD, camera, weather, trainer, Large Battles) is not required for multiplayer, but everyone must have it either on or off.

## License

Based on [Overgrowth's source code](https://github.com/WolfireGames/overgrowth). Unless otherwise noted, the code is licensed under `Apache-2.0`; see [LICENSE]. Third-party libraries keep their own licenses.

[Overgrowth]: https://overgrowth.wolfire.com
[Wolfire Games]: https://wolfire.com
[LICENSE]: LICENSE
