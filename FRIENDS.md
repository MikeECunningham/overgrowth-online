# Playing this fork with friends

Each person needs their own installed copy of Overgrowth and the same version of this fork. For now, building the source on each computer is the most dependable way to try these changes. Packaged releases and Steam friends integration are covered in [RELEASE.md](RELEASE.md).

## Get the source

Install Git, then run:

```sh
git clone https://github.com/MikeECunningham/overgrowth-online.git overgrowth-online
cd overgrowth-online
```

Use the same release tag or commit as the host. The inherited private Wolfire submodules are not required; clone without `--recurse-submodules`. Game assets are read from each player's installed game and are not included in the source.

The current local-multiplayer prerelease is `0.42.0-alpha.11`. Once its tag has been
published, select that exact source revision before configuring:

```sh
git switch --detach v0.42.0-alpha.11
```

Until a tag is published, ask the host for the commit to use. The in-game version
comes from this fork's `VERSION` file; the optional Overhaul mod is not required
to identify or play this version.

## Linux

Install the [Linux build dependencies](COMPILING.md#linux). On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake git pkg-config python3 mesa-common-dev libsdl2-dev libsdl2-net-dev libgtk2.0-dev libogg-dev libvorbis-dev libopenal-dev libjpeg-dev libpng-dev zlib1g-dev libbz2-dev libfreetype-dev libssl-dev libprotobuf-dev protobuf-compiler
```

From the checkout's root, substitute the folder found in Steam's **Properties > Installed Files > Browse**:

```sh
OVERGROWTH_GAME_DATA="/path/to/SteamLibrary/steamapps/common/Overgrowth"
cmake -S Projects -B Build -DCMAKE_BUILD_TYPE=Release -DBUILD_OGDA=OFF \
    -DENABLE_STEAMWORKS=OFF -DENABLE_GAMENETWORKINGSOCKETS=ON \
    -DAUX_DATA="$OVERGROWTH_GAME_DATA"
cmake --build Build --parallel 6
./Build/OvergrowthOnline.bin.x86_64 --working-dir "$PWD/"
```

Use a current OpenGL 4.6 GPU driver. OpenGL is the default; optional Vulkan setup is described in [COMPILING.md](COMPILING.md#optional-vulkan-shader-runtime).

The current development Linux binary requires **glibc 2.43**, so copying that executable to an older distribution may fail. Build locally for now; portable Linux downloads need a separate release build and runtime layout.

## Windows

Install CMake and Visual Studio 2022 with **Desktop development with C++** and a Windows SDK. Open a VS 2022 Developer PowerShell in the checkout's root:

```powershell
$gamePath = "C:/Program Files (x86)/Steam/steamapps/common/Overgrowth"
cmake -S Projects -B Build -G "Visual Studio 17 2022" -A x64 `
    -DBUILD_OGDA=OFF -DENABLE_STEAMWORKS=OFF -DENABLE_GAMENETWORKINGSOCKETS=ON `
    "-DAUX_DATA=$gamePath"
cmake --build Build --config Release --parallel 6
$repoPath = (Get-Location).Path
$profilePath = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Wolfire/OvergrowthOnline"
& ".\Build\Release\OvergrowthOnline.exe" --working-dir "$repoPath/" --write-dir "$profilePath/"
```

Replace `$gamePath` with your own installation folder. Keep the executable in `Build/Release` with its adjacent DLLs. Keep `--write-dir` to give the fork its own profile instead of the stock game's settings. This is a source-build recipe; a Windows download must also be compiled and tested on Windows before it is published.

## Host and connect

1. Launch the fork and open **Multiplayer**.
2. One person chooses **Host**, then selects the mode and map. Choose **Traditional** or **Scaling** at the top of the lobby; **Difficulty** contains the base preset and all scaling options, including physical equipment and optional time, level/match and performance rules. These progression rules start Off. Hover or focus controls for descriptions.
3. Everyone else chooses **Connect** and enters the host's IP address.
4. Add local players by pressing a button on their controllers. If a controller owns P1, **Enter** adds the keyboard/mouse as another player.
5. The host starts when everyone is present. The designated host can use **End game** to return everyone to the same lobby.

On a LAN, use the host's LAN address. Across the internet, the host may need to allow and forward **UDP 25744** to their computer. The source builds above use direct-IP networking. Steam invitations, friends-list joining and Valve relays use a separate Steamworks build; see [RELEASE.md](RELEASE.md).

Keep the same fork version and enabled mods on every machine. Local players can join in the lobby or during play; reconnecting a controller restores its existing player. Each player can open their own Start menu without stopping multiplayer; the designated host has the default shared-pause authority.

The host can use **Add bot** in the lobby or pause menu. Bots have roster entries
with names and teams, and the host can remove them. They do not add screens or
use controllers. In Arena, newly added bots join the next round; its usual
random team/FFA rules apply unless **Force lobby teams** is checked.
Outside Arena, friendly bots spread across human teammates, follow their crouch
and combat cues, and avoid starting fights while their assigned player is quiet.

Joining an already-running single-player game creates a local party. Its roster,
controller settings and screen assignments continue across campaign levels.
Campaign cutscenes fill the window and hold extra players hidden until the scene
ends, including players who join during it. Online hosting remains off until the
host chooses **Open to online players**.

The lobby's **Promote to leader** button transfers control of this computer's
screens, player management and shared settings. **Promote to host** transfers
match authority; selecting someone on another computer performs a pregame
server handoff. Wait for its completion before the original host disconnects.
For direct-IP play, the new host also needs a reachable multiplayer UDP port.
During play, **Teams** in the pause menu shows team assignments and lets the
local leader swap local screens. **Host controls teams** starts enabled;
disabling it allows personal team choices where the mode permits them.

## Updates and troubleshooting

After changing to the same new tag/commit as your friends, rerun the configure/build commands and restart the game. Release is already optimized; a Debug build is for troubleshooting, not the normal play configuration.

- **Missing assets:** `AUX_DATA` must name the installation folder containing `Data`, and `--working-dir` must name the source checkout. You can also supply `--game-data "/path/to/installed/Overgrowth"` when launching.
- **Cannot connect:** confirm matching versions/mods and the host's IP/UDP port. Start with a LAN test where possible.
- **Missing DLL or `.so`:** keep the executable's runtime files together and check [COMPILING.md](COMPILING.md). A Linux ABI mismatch requires a compatible build, not the game's asset files.
- **Logs:** Linux uses `~/.local/share/OvergrowthOnline/logfile.txt`; Windows uses `Documents/Wolfire/OvergrowthOnline/logfile.txt`. Include that file when reporting a problem.
