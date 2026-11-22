"""Locate native shader tools in an SDK, the local bootstrap, or PATH."""

import os
from pathlib import Path
import shutil


ENVIRONMENTS = {
    "glslangValidator": "GLSLANG_VALIDATOR",
    "spirv-val": "SPIRV_VAL",
    "spirv-cross": "SPIRV_CROSS",
}


def find_shader_tool(name, required=False):
    override = os.environ.get(ENVIRONMENTS[name])
    if override:
        found = shutil.which(override)
        if found:
            return found
        raise FileNotFoundError(f"{ENVIRONMENTS[name]} does not name an executable: {override}")

    executable = name + (".exe" if os.name == "nt" else "")
    candidates = []
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidates.extend(Path(sdk) / folder / executable for folder in ("bin", "Bin"))
    root = Path(__file__).resolve().parents[2]
    candidates.append(root / "Build/Dependencies/gpu/sysroot/usr/bin" / executable)
    if os.name == "nt":
        candidates.append(Path("R:/CoreFXProject/VulkanSDK/Bin") / executable)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    found = shutil.which(executable)
    if found:
        return found
    if required:
        raise FileNotFoundError(
            f"No native {name}. Set {ENVIRONMENTS[name]}, install the Vulkan SDK/tools, "
            "or run python3 Tools/ogmp/setup_linux_shaders.py."
        )
    return None
