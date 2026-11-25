"""Sandboxed engine instances for automated tests.

Every instance has its own write dir under test/run/<name>/, bridge port and
PID. Windows keeps the existing nonactivating window launcher. Linux uses the
native SDL driver with a hidden, nonactivating window and no pointer grabs.
Input is sent only through the engine's bridge. Set OGMP_WINDOW_ISOLATION=0
to show Linux test windows explicitly; SDL's offscreen driver does not support
the game's double-buffered OpenGL context.

Cleanup requires a recognized game executable and an explicit --write-dir
inside this checkout's test/run directory. The user's own game is never stopped.
"""

import os
import re
import shlex
import shutil
import signal
import subprocess
import time

from .bridge import Bridge

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace("\\", "/")
IS_WINDOWS = os.name == "nt"
WINDOWS_EXE_NAMES = ("OvergrowthOnline.exe", "OverhaulOnline.exe")
LINUX_EXE_NAMES = ("OvergrowthOnline.bin.x86_64", "OvergrowthOnline.x86_64", "OvergrowthOnline")
EXE_NAME = WINDOWS_EXE_NAMES[0] if IS_WINDOWS else LINUX_EXE_NAMES[0]
EXE_NAMES = WINDOWS_EXE_NAMES if IS_WINDOWS else LINUX_EXE_NAMES
EXE_FILTER = " OR ".join(f"Name='{n}'" for n in WINDOWS_EXE_NAMES)
EXE = ROOT + ("/Build/Release/" if IS_WINDOWS else "/Build/") + EXE_NAME


def exe_and_dir():
    """The exe and working dir to run: the GNS test build in the repo, or
    (OGMP_EXE / OGMP_WORKDIR) e.g. the Steam build in a release layout."""
    return os.environ.get("OGMP_EXE", EXE), os.environ.get("OGMP_WORKDIR", ROOT).rstrip("/")
TEST_DIR = ROOT + "/test"
RUN_DIR = TEST_DIR + "/run"
RESULTS = TEST_DIR + "/results"
TOOLS = ROOT + "/Tools"
BASE_CONFIG = os.environ.get("OGMP_BASE_CONFIG", "D:/OvergrowthAI/test/writedir/Data/config.txt" if IS_WINDOWS else ROOT + "/Data/defaults.txt")
BASE_PORT = 42710

STOCK_MODS = [("com-wolfire-arena", "0.0.1"), ("com-wolfire-drika", "1.0.0"), ("com-wolfire-lugaru-campaign", "1.0.0"),
              ("com-wolfire-machine-translations", "0.0.1"), ("com-wolfire-overgrowth-campaign", "1.0.0"),
              ("com-wolfire-overgrowth-core", "1.0.0"), ("com-wolfire-sandbox-levels", "0.0.1"),
              ("com-wolfire-timbles-therium", "rc"), ("com-wolfire-versus", "0.0.1")]

DEFAULT_KEYS = {
    "fullscreen": "0",
    "vsync": "0",
    "skip_loading_pause": "true",
    "aio_debug_log": "true",
}


def _powershell(*args):
    return subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass"] + list(args),
                          capture_output=True, text=True)


def _linux_window_isolation(directory, env):
    """Build a process-local SDL shim; never change desktop/window-manager state."""
    if env.get("OGMP_WINDOW_ISOLATION", "1") == "0":
        return
    library = os.path.join(directory, "linux_window_isolation.so")
    compiler = shlex.split(env.get("CC", "cc"))
    result = subprocess.run(compiler + ["-shared", "-fPIC", "-O2", TOOLS + "/ogmp/linux_window_isolation.c",
                                       "-o", library, "-ldl"], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("Cannot build Linux test-window isolation shim:\n" + result.stderr)
    # ld.so splits LD_PRELOAD on both colons and spaces. An inherited descriptor
    # also works when the checkout/profile path contains either character.
    descriptor = os.open(library, os.O_RDONLY)
    env["LD_PRELOAD"] = f"/proc/self/fd/{descriptor}" + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
    return descriptor


def _inside_run_dir(path):
    """Resolve symlinks and require a child, not the run directory itself."""
    parent = os.path.normcase(os.path.realpath(RUN_DIR))
    child = os.path.normcase(os.path.realpath(path))
    try:
        return child != parent and os.path.commonpath((parent, child)) == parent
    except ValueError:
        return False


def _profile_path(argv):
    profiles = []
    for index, arg in enumerate(argv):
        if arg == "--write-dir" and index + 1 < len(argv):
            profiles.append(argv[index + 1])
        if arg.startswith("--write-dir="):
            profiles.append(arg.split("=", 1)[1])
    # A relative path is interpreted against the other process's cwd, which
    # need not be ours. Ambiguous or unverified profile ownership is not enough
    # permission to stop a process.
    if len(profiles) != 1 or not os.path.isabs(profiles[0]):
        return None
    return os.path.normcase(os.path.realpath(profiles[0]))


def _owns_profile(argv):
    profile = _profile_path(argv)
    return profile is not None and _inside_run_dir(profile)


def _windows_argv(command):
    try:
        return [part.strip('"') for part in shlex.split(command, posix=False)]
    except ValueError:
        return []


def _linux_process(pid):
    try:
        directory = f"/proc/{pid}"
        if os.stat(directory).st_uid != os.getuid():
            return None
        with open(directory + "/cmdline", "rb") as stream:
            argv = [os.fsdecode(arg) for arg in stream.read().split(b"\0") if arg]
        if not argv:  # exited/zombie process
            return None
        executable = os.readlink(directory + "/exe").removesuffix(" (deleted)")
        return executable, argv
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def _matches_program(executable, argv, names):
    if os.path.basename(executable) in names:
        return True
    configured = exe_and_dir()[0]
    configured = os.path.realpath(shutil.which(configured) or configured)
    if os.path.realpath(executable) == configured:
        return True
    # OGMP_EXE can also be a test launcher script, whose /proc/exe is Python
    # or another interpreter. Match only its program argument, never arbitrary
    # text elsewhere in the command line.
    return any(os.path.realpath(arg) == configured for arg in argv[:2])


def _linux_processes(names):
    try:
        entries = os.listdir("/proc")
    except FileNotFoundError:
        return []
    found = []
    for entry in entries:
        if not entry.isdecimal():
            continue
        process = _linux_process(int(entry))
        if process and _matches_program(*process, names):
            found.append((int(entry), process[1]))
    return found


def our_processes():
    """(pid, command line) of every running test instance of ours."""
    if not IS_WINDOWS:
        return [(pid, shlex.join(argv)) for pid, argv in _linux_processes(EXE_NAMES) if _owns_profile(argv)]
    out = _powershell("-Command", f"Get-CimInstance Win32_Process -Filter \"{EXE_FILTER}\" | "
                                  "ForEach-Object { \"$($_.ProcessId)|$($_.CommandLine)\" }").stdout
    found = []

    for line in out.splitlines():
        if "|" not in line:
            continue
        pid, cmd = line.split("|", 1)
        if _owns_profile(_windows_argv(cmd)):
            found.append((int(pid), cmd))

    return found


def _kill_owned_pid(pid):
    """Recheck ownership at termination; never kill a caller-supplied PID alone."""
    if IS_WINDOWS:
        if not any(found == pid for found, _ in our_processes()):
            return False
        return subprocess.run(["taskkill.exe", "/F", "/PID", str(pid)], capture_output=True).returncode == 0
    descriptor = None
    try:
        # A pidfd pins the inspected process even if it exits and its numeric
        # PID is recycled between discovery and termination.
        if hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal"):
            descriptor = os.pidfd_open(pid)
        process = _linux_process(pid)
        if not process or not _matches_program(*process, EXE_NAMES) or not _owns_profile(process[1]):
            return False
        if descriptor is not None:
            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
        else:
            os.kill(pid, signal.SIGKILL)
        return True
    except (ProcessLookupError, PermissionError):
        return False
    finally:
        if descriptor is not None:
            os.close(descriptor)


def stop_all():
    stopped = []
    for pid, _ in our_processes():
        if _kill_owned_pid(pid):
            stopped.append(pid)
    return stopped


def user_game_running():
    """The user's own game is running: the stock exe (Overgrowth.exe, or
    Overgrowth_stock.exe once Overgrowth Online is installed), or their Overgrowth
    Online (an OvergrowthOnline.exe or, older, OverhaulOnline.exe that isn't one
    of our test instances). A dead
    process that a leftover handle keeps alive (a few KB) does not count."""
    if not IS_WINDOWS:
        names = EXE_NAMES + ("Overgrowth.bin.x86_64", "Overgrowth.x86_64", "Overgrowth", "Overgrowth_stock")
        return any(not _owns_profile(argv) for _, argv in _linux_processes(names))
    out = _powershell("-Command", "@(Get-Process Overgrowth,Overgrowth_stock -ErrorAction SilentlyContinue | "
                                  "Where-Object { $_.WorkingSet64 -gt 50MB }).Count").stdout.strip()
    if out not in ("", "0"):
        return True
    out = _powershell("-Command", f"Get-CimInstance Win32_Process -Filter \"{EXE_FILTER}\" | "
                                  "ForEach-Object { $_.CommandLine }").stdout
    return any(line.strip() and not _owns_profile(_windows_argv(line)) for line in out.splitlines())


class Instance:
    def __init__(self, name, index=0, level="", keys=None, width=800, height=450, mods=None, extra_args=""):
        self.name = name
        self.index = index
        self.level = level
        self.port = BASE_PORT + index
        self.dir = os.path.realpath(os.path.join(RUN_DIR, name)).replace("\\", "/")
        if not _inside_run_dir(self.dir):
            raise ValueError("Instance name must identify a directory inside test/run")
        self.writedir = self.dir + "/writedir/"
        self.keys = dict(DEFAULT_KEYS)
        self.keys.update({"screenwidth": str(width), "screenheight": str(height)})
        self.keys.update(keys or {})
        # OGMP_KEYS: config keys for every instance ("key=value;key=value"), e.g. a feature
        # switched on for any scenario
        for kv in os.environ.get("OGMP_KEYS", "").split(";"):
            if "=" in kv:
                k, v = kv.split("=", 1)
                self.keys[k.strip()] = v.strip()
        self.mods = mods or []
        self.extra_args = extra_args
        self.pid = None
        self.bridge = None
        self.process = None

    # Set-up ---------------------------------------------------------------------------

    def prepare(self, reuse_profile=False):
        if not _inside_run_dir(self.dir):
            raise ValueError("Instance profile no longer belongs to test/run")
        for pid, command in our_processes():
            argv = _windows_argv(command) if IS_WINDOWS else shlex.split(command)
            if _profile_path(argv) == os.path.normcase(os.path.realpath(self.writedir)):
                raise RuntimeError(f"{self.name}: profile is still in use by PID {pid}")
        if reuse_profile:
            if not os.path.isfile(self.writedir + "Data/config.txt"):
                raise ValueError("Cannot restart an unprepared test profile")
            return
        if os.path.isdir(self.dir):
            shutil.rmtree(self.dir)
        os.makedirs(self.writedir + "Data", exist_ok=True)
        lines = []
        if os.path.exists(BASE_CONFIG):
            with open(BASE_CONFIG, encoding="utf-8") as source:
                lines = source.read().splitlines()
        seen = set()
        out = []

        for line in lines:
            key = line.split(":", 1)[0].strip()
            if key in self.keys:
                out.append(f"{key}: {self.keys[key]}")
                seen.add(key)
            elif not key.startswith(("aio_test", "aiob_", "otr_", "ocam_", "ofin_", "ohud_")):
                out.append(line)

        out += [f"{k}: {v}" for k, v in self.keys.items() if k not in seen]
        with open(self.writedir + "Data/config.txt", "w", encoding="utf-8", newline="\n") as target:
            target.write("\n".join(out) + "\n")
        ids = list(STOCK_MODS)

        for mod in self.mods:
            dest = self.writedir + "Data/Mods/" + os.path.basename(mod.rstrip("/"))
            shutil.copytree(mod, dest)
            with open(mod + "/mod.xml", encoding="utf-8") as source:
                m = re.search(r"<Id>([^<]+)</Id>", source.read())
            ids.append((m.group(1), "0.1.0"))

        rows = "".join(f'    <ModInstance id="{i}" activated="true" modsource="local" version="{v}" />\n' for i, v in ids)
        with open(self.writedir + "modconfig.xml", "w", encoding="utf-8", newline="\n") as target:
            target.write('<?xml version="2.0" ?>\n<ActiveMods>\n' + rows + "</ActiveMods>\n")

    # Life cycle -----------------------------------------------------------------------

    def start(self, bridge_timeout=150.0, *, reuse_profile=False):
        self.prepare(reuse_profile)
        exe, work = exe_and_dir()
        if not IS_WINDOWS:
            args = [exe, "--write-dir", self.writedir, "--working-dir", work + "/", "--clear-log", "--no-dialogues", "--test-bridge", str(self.port)]
            args.extend(shlex.split(self.extra_args))
            args.extend(shlex.split(os.environ.get("OGMP_EXTRA", "")))
            if self.level:
                args.extend(["-l", self.level])
            env = os.environ.copy()
            env.setdefault("ALSOFT_DRIVERS", "null")
            env.setdefault("OGMP_VIRTUAL_CONTROLLERS_ONLY", "1")
            isolation_descriptor = _linux_window_isolation(self.dir, env)
            try:
                with open(self.dir + "/console.log", "w", encoding="utf-8") as output:
                    self.process = subprocess.Popen(args, cwd=work, env=env, stdin=subprocess.DEVNULL,
                                                    stdout=output, stderr=subprocess.STDOUT,
                                                    pass_fds=(() if isolation_descriptor is None else (isolation_descriptor,)))
            finally:
                if isolation_descriptor is not None:
                    os.close(isolation_descriptor)
            self.pid = self.process.pid
            try:
                self.bridge = Bridge(self.port).connect(timeout=bridge_timeout, alive=self.alive)
            except Exception:
                self.stop(graceful=False)
                raise
            return self
        # (quoted: a repo path with a space split into two arguments)
        args = (f"--write-dir \"{self.writedir}\" --working-dir \"{work}/\" --clear-log --no-dialogues "
                f"--test-bridge {self.port} {self.extra_args} {os.environ.get('OGMP_EXTRA', '')}")
        if self.level:
            args += f" -l {self.level}"
        # The launcher stays alive ~30 s after the start (it hands focus back
        # and keeps the window in its corner): read the PID, don't wait for it
        self.launcher = subprocess.Popen(
            ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", TOOLS + "/start_noactivate.ps1",
             "-Exe", exe, "-Dir", work, "-CmdArgs", args, "-Slot", str(self.index)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        seen = ""
        m = None

        for line in self.launcher.stdout:
            seen += line
            m = re.search(r"started pid=(\d+)", line)
            if m:
                break

        if m is None:
            raise RuntimeError(f"{self.name}: launch failed: {seen}")

        self.pid = int(m.group(1))
        self.bridge = Bridge(self.port).connect(timeout=bridge_timeout, alive=self.alive)
        return self

    def kill(self):
        """Hard kill (no quit): a crashed or unplugged player."""
        if self.bridge is not None:
            self.bridge.close()
            self.bridge = None
        if self.pid is not None:
            _kill_owned_pid(self.pid)
        if self.process is not None:
            self.process.wait(timeout=5)
        self.pid = None

    def alive(self):
        if self.process is not None:
            return self.process.poll() is None
        return self.pid is not None and any(pid == self.pid for pid, _ in our_processes())

    def stop(self, graceful=True):
        if self.bridge is not None:
            if graceful:
                try:
                    self.bridge.call("quit", timeout=5)
                except Exception:
                    pass
            self.bridge.close()
            self.bridge = None
        if graceful:
            end = time.time() + 8
            while time.time() < end and self.alive():
                time.sleep(0.5)

        if self.pid is not None and self.alive():
            _kill_owned_pid(self.pid)
        if self.process is not None:
            self.process.wait(timeout=5)

        self.pid = None

    # Output ---------------------------------------------------------------------------

    def log(self):
        path = self.writedir + "logfile.txt"
        return open(path, encoding="utf-8", errors="replace").read() if os.path.exists(path) else ""

    def capture(self, out):
        """Capture without focus; Linux uses the engine bridge, Windows its window."""
        if not IS_WINDOWS:
            if self.bridge is None:
                return False
            destination = os.path.abspath(out)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            try:
                self.bridge.call("screenshot", path=destination)
            except Exception:
                return False
            deadline = time.monotonic() + 10
            while not os.path.isfile(destination) and time.monotonic() < deadline:
                time.sleep(0.1)
            return os.path.isfile(destination)
        res = _powershell("-File", TOOLS + "/capture_pid.ps1", "-ProcessId", str(self.pid), "-Out", out.replace("/", "\\"))
        return res.returncode == 0


def log_errors(log):
    counts = {}
    for line in log.splitlines():
        if re.search(r"\[e\]|\[f\]|ERR:|WARN:", line):
            key = re.sub(r"^[0-9/: ]+", "", line)[:220]
            counts[key] = counts.get(key, 0) + 1
    return sorted(counts.items(), key=lambda kv: -kv[1])
