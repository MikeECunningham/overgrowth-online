"""Prepare native Vulkan shader tools and runtime libraries without sudo.

Debian/Ubuntu bootstrap: downloads distro packages into Build/Dependencies/gpu,
extracts them privately, and relinks their PIC static glslang archives into the
two shared C-API libraries the engine loads. No system packages are installed.
"""

import argparse
import ctypes
import json
from pathlib import Path
import platform
import shutil
import subprocess


def run(command, **kwargs):
    print("+", " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def prepare(root, download=True):
    if platform.system() != "Linux":
        raise RuntimeError("This bootstrap prepares native Linux libraries.")
    packages, sysroot, runtime = (root / name for name in ("packages", "sysroot", "runtime"))
    for directory in (packages, sysroot, runtime):
        directory.mkdir(parents=True, exist_ok=True)
    names = ["glslang-dev", "glslang-tools", "spirv-tools", "spirv-cross"]
    for optional in ("spirv-tools-dev", "spirv-tools-headers"):
        available = subprocess.run(["apt-cache", "show", optional], capture_output=True)
        if available.returncode == 0 and available.stdout:
            names.append(optional)
    if download:
        missing = [name for name in names if not list(packages.glob(name + "_*.deb"))]
        if missing:
            run(["apt-get", "download", *missing], cwd=packages)
    archives = sorted(packages.glob("*.deb"))
    if not archives:
        raise RuntimeError(f"No shader dependency packages in {packages}")
    for archive in archives:
        run(["dpkg-deb", "-x", archive, sysroot])
    bundled = Path(__file__).resolve().parents[2] / "Libraries/glslang-1.4.350/include/glslang"
    installed = sysroot / "usr/include/glslang"
    interfaces = ("Include/glslang_c_interface.h", "Include/glslang_c_shader_types.h", "Public/resource_limits_c.h")
    mismatches = [name for name in interfaces if not (installed / name).exists()
                  or (installed / name).read_bytes() != (bundled / name).read_bytes()]
    if mismatches:
        raise RuntimeError(
            "The distro glslang C API differs from this engine's bundled headers: " + ", ".join(mismatches)
            + ". Do not use these archives: a different C struct layout is unsafe. Use native shared libraries "
            "from the matching Vulkan SDK 1.4.350/glslang 16.2, or build matching glslang sources with "
            "BUILD_SHARED_LIBS=ON, and place both shared libraries beside the game."
        )
    libraries = list(sysroot.glob("usr/lib/**/libglslang.a"))
    if len(libraries) != 1:
        raise RuntimeError(f"Expected one native libglslang.a, found {libraries}")
    libdir = libraries[0].parent
    compiler = shutil.which("c++")
    if not compiler:
        raise RuntimeError("A native C++ compiler is required to prepare the shared runtime.")
    components = ["glslang", "MachineIndependent", "GenericCodeGen", "OSDependent", "OGLCompiler", "SPIRV"]
    core = []
    for name in components:
        archive = libdir / f"lib{name}.a"
        if not archive.exists():
            continue
        members = subprocess.check_output(["ar", "t", str(archive)], text=True).splitlines()
        # New glslang packages keep compatibility archives containing the same
        # dummy function; only older releases have real objects in these files.
        if any(member != "stub.cpp.o" for member in members):
            core.append(archive)
    spirv = [libdir / f"lib{name}.a" for name in ("SPIRV-Tools-opt", "SPIRV-Tools", "SPIRV-Tools-link")]
    if not all(path.exists() for path in spirv):
        raise RuntimeError("SPIRV-Tools development archives are missing from the private sysroot.")
    run([compiler, "-shared", "-Wl,--no-undefined", "-Wl,-soname,libglslang.so", "-o", runtime / "libglslang.so.tmp",
         "-Wl,--whole-archive", *core, "-Wl,--no-whole-archive", "-Wl,--start-group", *spirv, "-Wl,--end-group", "-pthread"])
    run([compiler, "-shared", "-Wl,--no-undefined", "-Wl,-soname,libglslang-default-resource-limits.so",
         "-o", runtime / "libglslang-default-resource-limits.so.tmp", "-Wl,--whole-archive",
         libdir / "libglslang-default-resource-limits.a", "-Wl,--no-whole-archive"])
    for name in ("libglslang.so", "libglslang-default-resource-limits.so"):
        (runtime / (name + ".tmp")).replace(runtime / name)
    compiler_library = ctypes.CDLL(str(runtime / "libglslang.so"))
    resources = ctypes.CDLL(str(runtime / "libglslang-default-resource-limits.so"))
    compiler_library.glslang_initialize_process.restype = ctypes.c_int
    resources.glslang_default_resource.restype = ctypes.c_void_p
    if compiler_library.glslang_initialize_process() != 1 or not resources.glslang_default_resource():
        raise RuntimeError("The native glslang C API did not initialize.")
    compiler_library.glslang_finalize_process()
    notice = sysroot / "usr/share/doc/glslang-dev/copyright"
    if notice.exists():
        shutil.copy2(notice, runtime / "glslang-copyright.txt")
    tools = sysroot / "usr/bin"
    run([tools / "glslangValidator", "--version"])
    run([tools / "spirv-val", "--version"])
    run([tools / "spirv-cross", "--revision"])
    manifest = {"packages": [archive.name for archive in archives], "runtime": str(runtime), "tools": str(tools)}
    (runtime / "toolchain.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Runtime: {runtime}\nTools: {tools}\nCopy both .so files and glslang-copyright.txt beside the game.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2] / "Build/Dependencies/gpu")
    parser.add_argument("--no-download", action="store_true", help="Reuse already downloaded distro packages.")
    args = parser.parse_args()
    prepare(args.root.resolve(), not args.no_download)
