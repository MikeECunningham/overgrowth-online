# Overgrowth Online

Overgrowth Online is an unofficial, community-made continuation of [Overgrowth], built on the open-source engine Wolfire Games released. It brings the game online and keeps developing the engine and the game further.

It is in active development.

Only code is here. The game's data (art, levels, sounds) comes from your own copy of Overgrowth, which you can buy from [Wolfire Games].

Overgrowth Online is not affiliated with or endorsed by Wolfire Games.

## Rendering compared to the original game

The renderer is being rebuilt under the original look. Every step is checked against screenshots of the build before it; the look changes in the table are deliberate.

| | Original Overgrowth | Overgrowth Online |
|---|---|---|
| Graphics API | OpenGL 3.2 | OpenGL 4.6, or Vulkan 1.3 (Settings > Screen > Renderer). If Vulkan fails to start, the game goes back to OpenGL |
| Platforms | Windows, macOS, Linux | Windows |
| Graphics resources | One shared vertex array, filtering stored on each texture, hundreds of loose uniforms looked up by name | Direct state access, immutable textures, sampler objects, vertex layouts, uniform blocks with fixed bindings, per-frame data in one ring buffer |
| Shaders | GLSL compiled by the driver; a variant missed at load time compiled during play | The same GLSL, also built to SPIR-V. On Vulkan the game's shaders ship precompiled, mods' shaders compile once and are cached, and pipelines are prepared during the menus and at level load |
| Character skinning | On the CPU. The optional GPU path was slow on NVIDIA GPUs in big battles (10-12 ms a frame with 96 characters on an RTX 5070 Ti) | On the GPU, the bones read from a buffer filled each frame |
| Depth buffer | 24-bit | Reversed-Z with 32-bit float depth: more precision in the distance |
| Anti-aliasing | MSAA (2x to 8x) | MSAA, or TAA on either renderer, with a sharpness slider. Characters and items write their own motion vectors; the rest of the scene is reprojected from the camera |
| Upscaling | None | On Vulkan: NVIDIA DLSS on GeForce RTX GPUs and AMD FSR 3.1 on any GPU, at native resolution as anti-aliasing (DLAA, FSR Native AA) or in Quality, Balanced, Performance and Ultra Performance modes. They use the same motion vectors as TAA |
| Bloom | Per colour channel: sunsets and backlit ears glowed pure red | Keeps each pixel's colour |
| Noise (dithering, grain) | A sine-based hash that comes out differently on each GPU and driver | An integer hash (PCG2D): the same on every GPU, on OpenGL and Vulkan |
| Errors | OpenGL errors hidden in release builds (an unused display list built for every item) | None on the test levels, checked with an OpenGL debug context and the Vulkan validation layer |

Next: GPU culling with multi-draw indirect and compute particles, then Intel XeSS and ray tracing on Vulkan.

## Compared to Wolfire's multiplayer prototype

Wolfire's online prototype (2021) was left unfinished. Overgrowth Online started from it and reworked it:

| | Wolfire's prototype | Overgrowth Online |
|---|---|---|
| Your character | Runs on the host: every move waits for the network | Runs on your machine: the network adds no input lag |
| Other characters | Full poses about 120 times a second, about 5 Mbit/s per player | Compact streams, about 35 kbit/s per player in a two-player game, smooth on any ping |
| Guests' frame rate | About half the host's: every streamed character's morph targets were looked up per vertex, twice a frame | Close to the host's |
| Hits | Decided on the host | Decided by the defender's own machine |
| Joining | Steam invites, or TCP by IP | Steam friends, a server browser, direct IP |
| Weapons | Pick-ups not shared | Held, thrown, stuck in a body or left on the ground: the same on every machine, tested with guests on different pings |
| Sounds | No footsteps from other players | Shared |
| Game modes | Any level, without co-op or PvP rules | Story co-op, arena PvP with bots |
| Who may change what | Any connected player could delete, spawn or move any object and rewrite player states and settings | The host checks every message: a player can only affect their own character; hits and grabs must be in reach |
| Malformed packets | Could crash the game | Dropped unread; tested with forged attacks |

## Build a level

Several people can build one level together, live. Multiplayer > Build a level offers an empty level, your saved levels and autosaves, mods' levels and the campaigns' levels. The level you pick is hosted as a build session and opens in the editor. Friends join the usual way: Steam friends, the server browser or direct IP. Each player who is allowed to build opens their own editor with F1 and goes back to play with F1. The game keeps running for everyone meanwhile. A line at the top of the screen says what F1 and Ctrl+S do and whether time is running.

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
- The Overhaul mod that comes with Overgrowth Online (AI, HUD, camera, weather, trainer, Large Battles) is not required for multiplayer, but everyone must have it either on or off.

## License

Based on [Overgrowth's source code](https://github.com/WolfireGames/overgrowth). Unless otherwise noted, the code is licensed under `Apache-2.0`; see [LICENSE]. Third-party libraries keep their own licenses.

[Overgrowth]: https://overgrowth.wolfire.com
[Wolfire Games]: https://wolfire.com
[LICENSE]: LICENSE
