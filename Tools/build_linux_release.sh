#!/usr/bin/env bash
# Native, incremental Linux build. Pass additional CMake options as arguments.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${OVERGROWTH_BUILD_DIR:-"$repo_dir/Build"}
build_jobs=${OVERGROWTH_BUILD_JOBS:-$(nproc)}
cmake_args=(-DCMAKE_BUILD_TYPE=Release -DBUILD_OGDA=OFF)
if [[ -n ${OVERGROWTH_GAME_DATA:-} ]]; then
    cmake_args+=("-DAUX_DATA=$OVERGROWTH_GAME_DATA")
fi

cmake -S "$repo_dir/Projects" -B "$build_dir" "${cmake_args[@]}" "$@"
cmake --build "$build_dir" --parallel "$build_jobs"
