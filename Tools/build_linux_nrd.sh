#!/usr/bin/env bash
# Build NRD's native Vulkan shaders and the Linux game. Additional arguments
# are forwarded to the game's CMake configuration just like build_linux_release.sh.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${OVERGROWTH_BUILD_DIR:-"$repo_dir/Build"}
build_jobs=${OVERGROWTH_BUILD_JOBS:-$(nproc)}
sdk_dir="$build_dir/Dependencies/NRD"
sdk_build_dir="$build_dir/Dependencies/nrd-vulkan"
sdk_version=v4.18.0
sdk_commit=8f12a22fc90e6a3c7f5da4c9c3103633c842963a

if [[ ! -e "$sdk_dir" ]]; then
    mkdir -p -- "$build_dir/Dependencies"
    git clone --depth 1 --branch "$sdk_version" https://github.com/NVIDIA-RTX/NRD.git "$sdk_dir"
fi
if [[ $(git -C "$sdk_dir" rev-parse HEAD) != "$sdk_commit" ]]; then
    echo "Expected NRD $sdk_version ($sdk_commit) at $sdk_dir." >&2
    exit 1
fi
sh_patch="$repo_dir/Tools/patches/nrd-4.18-reblur-vulkan-sh.patch"
if ! git -C "$sdk_dir" apply --reverse --check "$sh_patch" 2>/dev/null; then
    git -C "$sdk_dir" apply "$sh_patch"
fi

# Match the engine's R10G10B10A2 normal packing and linear roughness.
# The Vulkan renderer does not request compute shader quad derivatives.
cmake -S "$sdk_dir" -B "$sdk_build_dir" -DCMAKE_BUILD_TYPE=Release \
    -DSHADERMAKE_FIND_COMPILERS=ON -DSHADERMAKE_FIND_DXC=ON \
    -DSHADERMAKE_FIND_DXC_VK=OFF -USHADERMAKE_DXC_VK_PATH \
    -DNRD_STATIC_LIBRARY=OFF -DNRD_NRI=OFF \
    -DNRD_NORMAL_ENCODING=2 -DNRD_ROUGHNESS_ENCODING=1 \
    -DNRD_SUPPORTS_CHECKERBOARD=ON -DNRD_SUPPORTS_QUAD_INTRINSICS=OFF \
    -DNRD_EMBEDS_SPIRV_SHADERS=ON -DNRD_EMBEDS_DXIL_SHADERS=OFF -DNRD_EMBEDS_DXBC_SHADERS=OFF
cmake --build "$sdk_build_dir" --config Release --target NRD --parallel "$build_jobs"

exec "$repo_dir/Tools/build_linux_release.sh" "-DNRD_SDK=$(cd -- "$sdk_dir" && pwd)" "$@"
