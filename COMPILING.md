# Building Overgrowth Online

Each player needs an installed commercial copy of Overgrowth for its levels, art and sounds. This repository supplies the engine and its scripts/shaders. Set `AUX_DATA` to the **game's installation folder containing `Data/`**, rather than to `Data/` itself. It reads those assets without copying them into the checkout.

Clone the source normally. The inherited `Proprietary` and `Auxiliary` submodules point at private Wolfire repositories and are not needed for the standalone build below.

For a shorter player-oriented walkthrough, see [FRIENDS.md](FRIENDS.md). For packaged downloads and automated releases, see [RELEASE.md](RELEASE.md).

## Linux

Use a C++14 compiler, CMake 3.14 or newer, Git, Make and Python 3. Recent CMake 4 versions are supported. The default renderer needs a current driver with OpenGL 4.6 support; Vulkan 1.3 is optional.

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake git pkg-config python3 mesa-common-dev libsdl2-dev libsdl2-net-dev libgtk2.0-dev libogg-dev libvorbis-dev libopenal-dev libjpeg-dev libpng-dev zlib1g-dev libbz2-dev libfreetype-dev libssl-dev libprotobuf-dev protobuf-compiler
```

On Fedora:

```sh
sudo dnf install gcc-c++ cmake git make python3 'pkgconfig(gl)' 'pkgconfig(glu)' 'pkgconfig(sdl2)' 'pkgconfig(SDL2_net)' 'pkgconfig(gtk+-2.0)' 'pkgconfig(ogg)' 'pkgconfig(vorbis)' 'pkgconfig(openal)' 'pkgconfig(libjpeg)' 'pkgconfig(libpng)' 'pkgconfig(zlib)' 'pkgconfig(bzip2)' 'pkgconfig(freetype2)' openssl-devel protobuf-devel protobuf-compiler
```

From the checkout's root, substitute the installation folder found through Steam's **Properties > Installed Files > Browse**:

```sh
OVERGROWTH_GAME_DATA="/path/to/SteamLibrary/steamapps/common/Overgrowth"
cmake -S Projects -B Build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_OGDA=OFF \
    -DENABLE_STEAMWORKS=OFF \
    -DENABLE_GAMENETWORKINGSOCKETS=ON \
    -DAUX_DATA="$OVERGROWTH_GAME_DATA"
cmake --build Build --parallel 6
./Build/OvergrowthOnline.bin.x86_64 --working-dir "$PWD/"
```

The first configure downloads and builds pinned GameNetworkingSockets 1.4.0 source. For an existing matching source tree, pass `-DOVERGROWTH_GNS_SOURCE_DIR="/path/to/GameNetworkingSockets"`. An extracted Protobuf development SDK can be selected with `OVERGROWTH_DEPENDENCY_ROOT`; its default is `<build-directory>/Dependencies/sysroot`.

The incremental build helper runs the same Release configure/build workflow:

```sh
OVERGROWTH_GAME_DATA="/path/to/installed/Overgrowth" \
OVERGROWTH_BUILD_JOBS=6 \
    ./Tools/build_linux_release.sh
```

Set `OVERGROWTH_BUILD_DIR` to use another directory. Additional arguments go to CMake. The Makefile also supports `make`, `make run` and `make test`; set `CMAKE_ARGS='-DAUX_DATA=/path/to/installed/Overgrowth'` when configuring through Make.

### Optional Vulkan shader runtime

OpenGL is the default. Before selecting Vulkan, provide native `libglslang.so` and `libglslang-default-resource-limits.so` matching the bundled glslang 16.2 / Vulkan SDK 1.4.350 headers.

On Debian/Ubuntu, this helper can prepare them without installing system packages:

```sh
python3 Tools/ogmp/setup_linux_shaders.py
cmake -S Projects -B Build
cmake --build Build --parallel 6
```

It downloads packages into `Build/Dependencies/gpu`, extracts them privately, and checks their C API headers against this engine's headers. If it rejects a version mismatch, use the matching native Vulkan SDK libraries or build matching glslang sources with `BUILD_SHARED_LIBS=ON`. The package helper is specific to Debian/Ubuntu.

CMake copies the prepared libraries and their copyright notice beside the executable. For a different build directory, select the prepared directory with `-DOVERGROWTH_SHADER_RUNTIME_DIR="/path/to/runtime"`. The engine also searches the executable's directory, `$VULKAN_SDK/lib`, `$VULKAN_SDK/lib64`, and the normal loader path.

The Linux renderer includes OpenGL, Vulkan and TAA. The current DLSS, FSR and NRD integrations use Windows SDK binaries and are excluded from the Linux build.

## Windows

Install Git, CMake and Visual Studio 2022 with **Desktop development with C++**, the MSVC compiler and a Windows SDK. Python 3 is needed for the automated regression tools. Use a VS 2022 Developer PowerShell and run these commands from the checkout's root:

```powershell
$gamePath = "C:/Program Files (x86)/Steam/steamapps/common/Overgrowth"
cmake -S Projects -B Build -G "Visual Studio 17 2022" -A x64 `
    -DBUILD_OGDA=OFF `
    -DENABLE_STEAMWORKS=OFF `
    -DENABLE_GAMENETWORKINGSOCKETS=ON `
    "-DAUX_DATA=$gamePath"
cmake --build Build --config Release --parallel 6
$repoPath = (Get-Location).Path
$profilePath = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "Wolfire/OvergrowthOnline"
& ".\Build\Release\OvergrowthOnline.exe" --working-dir "$repoPath/" --write-dir "$profilePath/"
```

Replace `$gamePath` with the installation folder found through Steam. Visual Studio uses multiple configurations in one build directory: `--config Release` selects the optimized executable. Run it with the DLLs CMake places beside it.

For Vulkan, install the matching Vulkan SDK and expose `VULKAN_SDK`, or put `glslang.dll` and `glslang-default-resource-limits.dll` beside `OvergrowthOnline.exe`. The optional DLSS, FSR and NRD SDKs are separate from the default build.

The Windows recipe follows the checked-in CMake project. Changes made and tested on Linux still need Windows compilation and runtime checks before publishing a Windows package.

## Build configurations and distribution

| Configuration | Intended use |
| --- | --- |
| `Debug` | Debugging with optimization disabled and development checks enabled. |
| `RelWithDebInfo` | Optimized code with symbols and the project's `OG_DEBUG` development define. |
| `Release` | Optimized gameplay and distribution testing. Symbols may still be retained. |

The native Linux Release configuration uses `-O3 -DNDEBUG` and deliberately retains `-g` and frame pointers for crash analysis. Debug symbols do not turn it into an unoptimized Debug build. On Windows, Release uses the MSVC Release configuration and produces debugging symbols too.

For a separate Linux debug build, use `-B Build-debug -DCMAKE_BUILD_TYPE=Debug` with the same asset/dependency options. On Windows, build with `--config Debug` and run the executable from `Build/Debug/`.

`DEPLOY=ON` changes deployment/working-directory behavior; it is separate from compiler optimization. `BUILD_SERVER=ON` also enables Steamworks and archive-related settings, so it is not the switch to use just to get a faster local build. Unit tests and the explicitly enabled test bridge are currently compiled into the game; the bridge stays inactive unless launched with `--test-bridge <port>`.

A runnable local build is not automatically a portable package. In particular, the current development Linux executable requires **glibc 2.43**, along with its other native libraries. Building on each friend's machine avoids shipping that host ABI requirement. A redistributable Linux build needs a declared compatible baseline and runtime packaging; see [RELEASE.md](RELEASE.md).

## Assets, profiles and networking

Always launch with `--working-dir` pointing at the checkout so that the fork's scripts and shaders take precedence. To select another commercial installation at runtime, add `--game-data "/path/to/installed/Overgrowth"`; that path also names the folder containing `Data`.

The default Linux profile is `~/.local/share/OvergrowthOnline/`, or `$XDG_DATA_HOME/OvergrowthOnline/`. The Windows launch command above explicitly selects `Documents/Wolfire/OvergrowthOnline`, keeping settings, saves, logs and caches separate from the original game. Keep that `--write-dir` argument when launching a Windows source build: its inherited native default still names the stock game's profile. Use another `--write-dir "/path/to/profile/"` to isolate test clients.

These commands build standalone networking: host or connect by IP using UDP port **25744** by default. The host may need to allow/forward that UDP port for internet connections. Steam friends, Steam lobby discovery, invitations and Valve relays require a Steamworks build and its SDK/runtime; see [RELEASE.md](RELEASE.md). Installing the commercial game through Steam supplies assets but does not enable those integration features in a standalone executable.

All participants need matching fork versions and mods. A missing-data popup usually means `AUX_DATA` or `--game-data` points at the wrong folder, or `--working-dir` selected stock scripts instead of this checkout. A missing DLL/`.so` is a runtime-library issue; preserve the executable's adjacent libraries and check the platform's dependencies above.

## Checks

On Linux, from the checkout:

```sh
./Build/OvergrowthOnline.bin.x86_64 --run-unit-tests --no-dialogues
python3 Tools/ogmp.py suite smoke
python3 Tools/ogmp.py run local_lobby lobby_lifecycle player_menus mode_teams
```

On Windows, use `Build/Release/OvergrowthOnline.exe --run-unit-tests --no-dialogues` and `python Tools/ogmp.py ...` for the same scenarios. Tests need the configured commercial assets and a working display. Instances use separate profiles under `test/run`; results go to `test/results`. Set `OGMP_KEYS=renderer=vulkan` to exercise Vulkan after preparing its runtime.

## macOS

The fork's newer renderer and networking have not been ported and verified on macOS. The inherited macOS CI/build files do not establish support for this version.
