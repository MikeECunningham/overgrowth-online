# Publishing this fork

This fork is published at <https://github.com/MikeECunningham/overgrowth-online>.
The current version is **0.42.0-alpha.15**, a testing prerelease. See [VERSION](VERSION)
for the authoritative version, [FRIENDS.md](FRIENDS.md) for building and playing,
and [COMPILING.md](COMPILING.md) for dependency details.

Alpha.12 adds optional campaign and Arena XP, between-fight shops, persistent
loadouts and campaign loops. Arena fills small mode-managed matches with house
fighters. Campaign checkpoints keep defeated incidental guards behind the
party and recover stalled defeat objectives. The host can choose Legacy,
Weighted or Localized armor. Controllers can scroll menus with the right stick.
Alpha.13 updates shop meshes and locks enemy strength to gameplay boundaries.
Alpha.14 adds four armor materials, stealth upgrades and permission-aware menu pausing.
Alpha.15 adds host-local save cards, persistent lobby slots, selectable AI rates,
and an Arena shop after the match-winning camera. Protocol **16** synchronizes slot
assignments, new loadout choices and Nightmare scaling rates.
Distribute the matching executable and Data overrides to every participant.

## Versioning

`VERSION` identifies this fork, independently of the optional Overhaul mod.
CMake embeds it into the executable. The game displays the fork version and
Git build metadata; multiplayer checks the fork version and the separately
maintained local-multiplayer protocol revision.

- Use `0.42.0-alpha.1`, `0.42.0-alpha.2`, etc. for testing builds.
- Bump `VERSION` for each distributed revision. Ship the matching scripts and
  shaders with that executable; a matching number cannot detect files someone
  manually replaced afterward.
- Bump the central protocol revision when changing the network format or its
  interpretation. Keep the same version and protocol on Windows and Linux
  builds made from the same release commit.
- Keep the upstream asset/mod compatibility version (`0.41.36`) separate from
  the fork's release number. Publishing a new fork tag must not change which
  existing assets and mods the engine accepts.
- `BUILD_ID=-1` means a local/development build identifier. It does **not** mean
  the compiler used Debug optimization. If assigning a release `BUILD_ID`, use
  a consistent value for tracing artifacts back to their release build. Network
  compatibility is determined by the fork version and protocol, not that ID.

Once a release commit has passed its checks, create its corresponding version
tag, such as `v0.42.0-alpha.15`. Create a GitHub **prerelease** from that tag and
attach the tested artifacts.
Do not move a published tag to different code; publish another version instead.

## What the current builds contain

The Linux build used during development is already **Release**, with `-O3` and
`-DNDEBUG`. It also retains debugging symbols and frame pointers for useful
crash reports. Symbols increase the file size; they do not turn it into an
unoptimized Debug build. Unit tests and the opt-in localhost test bridge are
still compiled in. The bridge runs only when explicitly requested.

The current configuration is the **standalone GameNetworkingSockets** flavor.
It supports direct-IP multiplayer. It has Steamworks disabled, so it does not
provide Steam friends, lobby discovery, invites or Valve relays. Buying the
Steam game provides the assets but does not enable those features in a build.

An optimized executable is one part of a release. Portable dependency packaging,
clean-machine installation and Steam-specific behavior are separate checks.

## Release procedure

1. **Freeze a revision.** Review and commit the source, scripts, shaders, version
   and documentation. Build every artifact from that same commit.
2. **Build in a clean directory.** Use Release configuration and an agreed
   platform/runtime baseline. Keep standalone and Steamworks builds in separate
   directories. Do not use a developer's absolute `AUX_DATA` path as the only
   way for a distributed package to locate assets; provide a launcher using
   `--game-data` and the player's own Overgrowth installation.
3. **Run automated checks.** For a Linux build and a machine with the commercial
   assets and a working display:

   ```sh
   ./Build/OvergrowthOnline.bin.x86_64 --run-unit-tests --no-dialogues
   python3 Tools/ogmp.py suite smoke
   python3 Tools/ogmp.py run local_lobby lobby_lifecycle player_menus mode_teams
   python3 Tools/ogmp.py run lobby_authority lobby_rollback stucco_solo cave_spawns loose_weapon_playback
   python3 Tools/ogmp.py run newcampaign_local newcampaign_roles newcampaign_main_removal
   python3 Tools/ogmp.py run newcampaign_cinematic_retry
   python3 Tools/ogmp.py run lobby_bots roster_bots_arena bot_migration
   python3 Tools/ogmp.py run lobby_role_ui connection_cancel
   python3 Tools/ogmp.py run difficulty_ui difficulty_hud mode_settings_lobby mode_challenge
   python3 Tools/ogmp.py run performance_solo campaign_equipment campaign_menu_resume
   python3 Tools/ogmp.py run session_slots session_saves session_saves_solo arena_saves
   python3 Tools/ogmp.py run difficulty_rates progression_weapons_xp ice_cliffs_retries
   ```

   Close a normally running game before using the test runner. Repeat relevant
   rendering tests with `OGMP_KEYS=renderer=vulkan`; enable the installed Vulkan
   validation layer when checking synchronization. Windows uses the executable
   under `Build/Release/` and the same Python test runner.
4. **Test the packaged layout.** Use a fresh profile and a clean machine/VM with
   its own game assets. Check asset discovery, first launch, OpenGL/Vulkan,
   controller reconnects, independent menus, mixed local/remote players,
   mode-managed/forced teams, campaign chapter progression with a retained party,
   joining during dialogue, End game and another match.
5. **Package only redistributable content.** Include the executable, this fork's
   data overrides, needed runtime libraries, launcher/instructions and license
   notices. Keep symbols as a separate diagnostic artifact if desired. Exclude
   purchased game assets, profiles, caches and SDK source.
6. **Publish and test the download.** Attach per-platform/per-networking-flavor
   archives to a versioned GitHub prerelease. Download the uploaded artifact
   and test it, rather than only testing the build directory.

### Linux binary portability

The current workstation-built executable requires **GLIBC 2.43** and recent
libstdc++/other shared libraries. Copying it to an older distribution will not
make those dependencies available. Source builds on friends' own machines are
the supported sharing path in [FRIENDS.md](FRIENDS.md) for now.

Before promising a downloadable Linux binary, choose the oldest runtime you
intend to support, build within that baseline/container, package the appropriate
runtime dependencies, and test on that baseline. The Vulkan shader helper's
locally extracted libraries also need this compatibility check. Build symbols
and `ldd`/`readelf --version-info` output are useful release artifacts.

### Existing automation needs adaptation

The checked-in GitHub workflows are inherited compile/check workflows, not a
working release publisher for this fork. They use old runner/dependency
assumptions; the Steam workflow also has a `DBUILD_SERVER` typo. They need an
updated platform matrix, dependency setup, checks, artifact uploads and a
tag-triggered release job before relying on green CI as a release gate.

`Tools/package/make_package.py` is a Windows-specific packaging prototype. It
contains the upstream author's `D:/` and `R:/` paths, expects an external
Overhaul checkout, and requires shader-cache preparation. The configure/build
batch files also contain machine-specific paths. `DO_DIST_ARCHIVE` is an older
binary/library archive step and does not supply this fork's complete overlay.
These scripts need adaptation before use; the generic CMake commands in
[FRIENDS.md](FRIENDS.md) avoid those hardcoded build paths.

## Steam friends, invites and relays

The source already implements Steam friends, public/friends-only lobbies,
invites, rich presence and Steam-backed P2P networking. The missing work for this
fork is building, packaging and testing that flavor on the target platforms.
No Steamworks SDK is currently installed in the development checkout, and the
recent multiplayer tests used standalone UDP.

Download the SDK through Valve's [Steamworks SDK page](https://partner.steamgames.com/doc/sdk)
and accept its terms. Keep it outside Git, for example at
`Proprietary/steamworks/sdk`. Use headers and runtime libraries from the **same**
SDK release. Do not combine current headers with the old Steam runtime already
present under `Libraries/Linux`. Valve permits distribution of the SDK's
designated redistributable binaries under its
[SDK access agreement](https://partner.steamgames.com/documentation/sdk_access_agreement?l=english);
that is separate from publishing the SDK source.

The following Linux configuration is a starting point, **not a verified Steam
release recipe**:

```sh
cmake -S Projects -B Build_steam \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_OGDA=OFF \
    -DENABLE_STEAMWORKS=ON \
    -DSTEAMWORKS_SDK="/absolute/path/to/sdk" \
    -DOVERGROWTH_SHADER_RUNTIME_DIR="$PWD/Build/Dependencies/gpu/runtime" \
    -DAUX_DATA="/path/to/SteamLibrary/steamapps/common/Overgrowth"
cmake --build Build_steam --parallel 6
```

For Windows, use the generic Visual Studio CMake configuration in `FRIENDS.md`
with a separate `Build_steam` directory and the same Steamworks/SDK options,
then build with `--config Release`. An SDK version must be recorded once a
successful Steam build has been tested.

Valve's [API overview](https://partner.steamgames.com/doc/sdk/api) describes the
required Steam runtime: `steam_api64.dll` on Windows or `libsteam_api.so` on
Linux. Linux's current CMake path links the SDK library but does not finish
runtime copying/launcher setup, so packaging must supply the matching library
in a directory the loader actually searches.

The existing integration assumes Overgrowth's App ID **25000**. Steam must be
running with the appropriate game license and application context. Merely
starting the standalone executable from a Steam shortcut does not add missing
Steamworks code. For development, Valve documents `steam_appid.txt` in the
working directory; this project's `--working-dir` option changes that directory.
For an actual Steam-depot release, follow Valve's instructions to omit that
development override and use the launch-provided App ID. A separate Steam
product requires its own application setup and the necessary distribution
rights; this repository is not permission to publish under Wolfire's app.

Before labeling a Steam build ready, test with **two Steam accounts on two
machines**: overlay, friend discovery, invitations while the game is running,
invitation launch when it is closed, lobby entry before loading, relay/P2P
connectivity without manual port forwarding, multiple local players per peer,
End game and reconnect. Test Windows/Linux together if distributing both.
Closed-game invites must launch the fork rather than the stock executable;
the existing Windows launcher illustrates that requirement but is not a
finished Linux solution.

Standalone GameNetworkingSockets does not include Valve relay access. See
Valve's [Steam Datagram Relay documentation](https://partner.steamgames.com/doc/features/multiplayer/steamdatagramrelay)
for how Steam-backed P2P and relay services fit together.
