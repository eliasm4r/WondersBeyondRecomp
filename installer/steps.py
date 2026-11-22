import collections
import ctypes
import hashlib
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

MAC = sys.platform == "darwin"
LINUX = sys.platform.startswith("linux")
ARM = platform.machine().lower() in ("aarch64", "arm64")
GAME = "pokepark2"
DISC_ID = "S2LP01"
OLD_FOLDER = Path("games") / GAME
FOLDER_NAME = "WondersBeyondRecomp"
EXECUTABLE = "pokepark2" if MAC or LINUX else "pokepark2.exe"
SHORTCUT_NAME = "PokePark 2 Wonders Beyond"
APP_IDENTIFIER = "io.github.eliasm4r.wondersbeyondrecomp"
NEEDED_GIGABYTES = 4
LONGEST_FOLDER = 100
SOURCES = Path("installer") / "sources.zip"
BUILT_FROM = Path("build") / "sources.sha256"

TOOLS = [
    {"name": "nodtool 1.4.4", "url": "https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-windows-x86_64.exe",
     "sha256": "fb3203a68a59fa19ba9de0aa2f8c339396b6e4c5f9274d0858d302074cdedf8b", "file": "nodtool.exe", "folder": "nodtool", "kind": "single",
     "path": "nodtool", "megabytes": 5},
    {"name": "CMake 4.4.3", "url": "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip",
     "sha256": "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab", "file": "cmake-4.4.3-windows-x86_64.zip", "folder": "cmake",
     "kind": "zip", "path": "cmake/cmake-4.4.3-windows-x86_64/bin", "megabytes": 50},
    {"name": "Ninja 1.13.2", "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "sha256": "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", "file": "ninja-win.zip", "folder": "ninja", "kind": "zip",
     "path": "ninja", "megabytes": 1},
    {"name": "MinGW-w64 GCC 15.2", "url": "https://nuwen.net/files/mingw/mingw-20.0.exe",
     "sha256": "bce8bdaa095848561488d34fd80371a1f36eb0a590202eb534f32e026e5cd8ef", "file": "mingw-20.0.exe", "folder": "mingw", "kind": "tar",
     "path": "mingw/MinGW/bin", "megabytes": 110},
]

if MAC:
    TOOLS = [
        {"name": "nodtool 1.4.4", "url": "https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-macos-arm64",
         "sha256": "141bc6dfeef6e88197fd75fcb85ee3c5ca64a8b9dcda895af44fbbda29726b78", "file": "nodtool", "folder": "nodtool", "kind": "single",
         "path": "nodtool", "megabytes": 4},
        {"name": "CMake 4.4.3", "url": "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-macos-universal.tar.gz",
         "sha256": "0c5d65251c14cc884bfa16bdbed3c263ce5bffe2e21c0d0d00962cb0610464fa", "file": "cmake-4.4.3-macos-universal.tar.gz", "folder": "cmake",
         "kind": "tar", "path": "cmake/cmake-4.4.3-macos-universal/CMake.app/Contents/bin", "megabytes": 86},
        {"name": "Ninja 1.13.2", "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip",
         "sha256": "c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b", "file": "ninja-mac.zip", "folder": "ninja", "kind": "zip",
         "path": "ninja", "megabytes": 1},
    ]

if LINUX:
    machine = "aarch64" if ARM else "x86_64"
    TOOLS = [
        {"name": "nodtool 1.4.4", "url": f"https://github.com/encounter/nod/releases/download/v1.4.4/nodtool-linux-{machine}",
         "sha256": "4180f9501dc0de2fb4841c865604fad91899798280f49015e7be8f89890ee37e" if ARM else "cf20828d1a437ca14ed7acfd5545e1a0e62c082f5a77240a399aa4f172119fc4",
         "file": "nodtool", "folder": "nodtool", "kind": "single", "path": "nodtool", "megabytes": 4},
        {"name": "CMake 4.4.3", "url": f"https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-linux-{machine}.tar.gz",
         "sha256": "2efc974dbd63b4444c0e8494b92f2e80c2d7e635b4b80eac2916985ddd8f72a6" if ARM else "d6c83076c575bc00b823522ac974bda66d0af05d6ddc30e739c12385cf32c6cc",
         "file": f"cmake-4.4.3-linux-{machine}.tar.gz", "folder": "cmake", "kind": "tar", "path": f"cmake/cmake-4.4.3-linux-{machine}/bin", "megabytes": 62},
        {"name": "Ninja 1.13.2", "url": "https://github.com/ninja-build/ninja/releases/download/v1.13.2/" + ("ninja-linux-aarch64.zip" if ARM else "ninja-linux.zip"),
         "sha256": "fd2cacc8050a7f12a16a2e48f9e06fca5c14fc4c2bee2babb67b58be17a607fc" if ARM else "5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6",
         "file": "ninja-linux-aarch64.zip" if ARM else "ninja-linux.zip", "folder": "ninja", "kind": "zip", "path": "ninja", "megabytes": 1},
        {"name": "Zig 0.17.1", "url": f"https://ziglang.org/download/0.17.1/zig-{machine}-linux-0.17.1.tar.xz",
         "sha256": "a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795" if ARM else "3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a",
         "file": f"zig-{machine}-linux-0.17.1.tar.xz", "folder": "zig", "kind": "tar", "path": f"zig/zig-{machine}-linux-0.17.1", "megabytes": 55},
    ]

UNIX = MAC or LINUX
SDL_BUNDLE = Path("installer") / "sdl3"
HIDDEN = 0x08000000


class Stopped(Exception):
    pass


class Failure(Exception):
    pass


def default_folder():
    if UNIX:
        return str(Path.home() / FOLDER_NAME)
    local = os.environ.get("LOCALAPPDATA", "")
    if local and local.isascii():
        return str(Path(local) / FOLDER_NAME)
    return str(Path(os.environ.get("SystemDrive", "C:") + "\\") / FOLDER_NAME)


def game_extracted(root):
    return any((Path(root) / folder / "extracted" / "sys" / "main.dol").is_file() for folder in ("game", OLD_FOLDER))


def sources_digest(bundle):
    value = hashlib.sha256()
    with open(Path(bundle) / SOURCES, "rb") as file:
        while block := file.read(1 << 20):
            value.update(block)
    return value.hexdigest()


def built_from_these(root):
    try:
        return (Path(root) / BUILT_FROM).read_text().strip() == sources_digest(root) and (Path(root) / EXECUTABLE).is_file()
    except OSError:
        return False


def move_old_folder(root):
    old = Path(root) / OLD_FOLDER
    if not old.is_dir():
        return
    folder = Path(root) / "game"
    folder.mkdir(parents=True, exist_ok=True)
    for name in ("disc", "extracted", "nand", "backups", "textures", "settings.ini", "stats.ini"):
        if (old / name).exists() and not (folder / name).exists():
            os.replace(old / name, folder / name)
    for name in ("analysis", "res"):
        shutil.rmtree(old / name, ignore_errors=True)
    for name in ("CMakeLists.txt", "game.cpp", "game.toml"):
        (old / name).unlink(missing_ok=True)
    for empty in (old, old.parent):
        try:
            empty.rmdir()
        except OSError:
            pass


def free_gigabytes(folder):
    path = Path(folder)
    while not path.exists() and path.parent != path:
        path = path.parent
    try:
        return shutil.disk_usage(path).free / (1 << 30)
    except OSError:
        return NEEDED_GIGABYTES


def built_elsewhere(root):
    try:
        cache = (Path(root) / "build" / "out" / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    for line in cache.splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            made = line.split("=", 1)[1].strip()
            return os.path.normcase(os.path.realpath(made)) != os.path.normcase(os.path.realpath(root))
    return False


def compile_jobs():
    cores = os.cpu_count() or 1
    if LINUX:
        try:
            with open("/proc/meminfo", encoding="ascii") as file:
                kilobytes = next(int(line.split()[1]) for line in file if line.startswith("MemTotal:"))
        except (OSError, StopIteration, ValueError):
            return cores
        return max(1, min(cores, ((kilobytes >> 10) - 1536) // 640))
    if MAC:
        try:
            megabytes = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout) >> 20
        except ValueError:
            return cores
        return max(1, min(cores, (megabytes - 1536) // 640))

    class Memory(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong),
                    ("page_total", ctypes.c_ulonglong), ("page_free", ctypes.c_ulonglong), ("virtual_total", ctypes.c_ulonglong),
                    ("virtual_free", ctypes.c_ulonglong), ("extended", ctypes.c_ulonglong)]

    memory = Memory()
    memory.length = ctypes.sizeof(Memory)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)):
        return cores
    megabytes = memory.total >> 20
    return max(1, min(cores, (megabytes - 1536) // 640))


def create_shortcut(link, target, folder):
    class Guid(ctypes.Structure):
        _fields_ = [("a", ctypes.c_ulong), ("b", ctypes.c_ushort), ("c", ctypes.c_ushort), ("d", ctypes.c_ubyte * 8)]

    ole32 = ctypes.OleDLL("ole32")

    def guid(text):
        value = Guid()
        ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(value))
        return value

    def method(pointer, index, *types):
        table = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))[0]
        address = ctypes.cast(table, ctypes.POINTER(ctypes.c_void_p))[index]
        return ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, *types)(address)

    ole32.CoInitialize(None)
    shell_link = ctypes.c_void_p()
    ole32.CoCreateInstance(ctypes.byref(guid("{00021401-0000-0000-C000-000000000046}")), None, 1,
                           ctypes.byref(guid("{000214F9-0000-0000-C000-000000000046}")), ctypes.byref(shell_link))
    try:
        method(shell_link, 20, ctypes.c_wchar_p)(shell_link, str(target))
        method(shell_link, 9, ctypes.c_wchar_p)(shell_link, str(folder))
        method(shell_link, 17, ctypes.c_wchar_p, ctypes.c_int)(shell_link, str(target), 0)
        file = ctypes.c_void_p()
        method(shell_link, 0, ctypes.c_void_p, ctypes.c_void_p)(shell_link, ctypes.byref(guid("{0000010B-0000-0000-C000-000000000046}")), ctypes.byref(file))
        try:
            method(file, 6, ctypes.c_wchar_p, ctypes.c_int)(file, str(link), 1)
        finally:
            method(file, 2)(file)
    finally:
        method(shell_link, 2)(shell_link)


def shell_folder(identifier):
    buffer = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(None, identifier, None, 0, buffer)
    return Path(buffer.value)


class Install:
    def __init__(self, root, disc, bundle, shortcut, texts, report, menu=True):
        self.root = Path(root)
        self.disc = disc
        self.bundle = Path(bundle)
        self.shortcut = shortcut
        self.menu = menu
        self.texts = texts
        self.report = report
        self.stop = threading.Event()
        self.process = None
        self.log = None
        self.python = str(self.bundle / "python" / "bin" / "python3") if UNIX else str(self.bundle / "python" / "python.exe")
        self.launch = str(Path.home() / "Applications" / (SHORTCUT_NAME + ".app")) if MAC else str(self.root / EXECUTABLE)
        self.tools = self.root / "toolchain"
        self.environment = dict(os.environ)
        folders = [str(self.tools / Path(tool["path"])) for tool in TOOLS] + [str(Path(self.python).parent)]
        self.environment["PATH"] = os.pathsep.join(folders + [os.environ.get("PATH", "")])
        self.environment["PYTHONUTF8"] = "1"
        self.environment["PYTHONDONTWRITEBYTECODE"] = "1"

    def cancel(self):
        self.stop.set()
        process = self.process
        if process and process.poll() is None:
            if UNIX:
                process.kill()
            else:
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], creationflags=HIDDEN, capture_output=True)

    def plan(self):
        t = self.texts
        steps = [(t["step_sources"], 1, self.copy_sources)]
        for tool in TOOLS:
            if self.tool_ready(tool):
                continue
            steps.append((t["step_download"].format(name=tool["name"]), max(1, tool["megabytes"] // 10), lambda move, tool=tool: self.download(tool, move)))
            steps.append((t["step_unpack"].format(name=tool["name"]), max(1, tool["megabytes"] // 20), lambda move, tool=tool: self.unpack(tool)))
        if not game_extracted(self.root):
            steps.append((t["step_verify"], 4, self.verify_disc))
            steps.append((t["step_extract"], 4, self.extract_disc))
        steps.append((t["step_verify"], 0.2, self.identify_disc))
        steps.append((t["step_sdl"], 1, lambda move: self.run([self.python, "tools/fetch_sdl.py"])))
        steps.append((t["step_code"], 8, lambda move: self.run([self.python, "recompiler/recomp.py"])))
        steps.append((t["step_sound"], 1, lambda move: self.run([self.python, "recompiler/dsp/recomp_dsp.py"])))
        steps.append((t["step_configure"], 1, self.configure))
        steps.append((t["step_build"], 60, self.build))
        steps.append((t["step_finish"], 0.2, self.finish))
        return steps

    def execute(self):
        title = ""
        try:
            (self.root / "logs").mkdir(parents=True, exist_ok=True)
            self.log = open(self.root / "logs" / "install.log", "a", encoding="utf-8", errors="replace")
            self.note(time.strftime("install started %Y-%m-%d %H:%M:%S"))
            move_old_folder(self.root)
            steps = self.plan()
            total = sum(weight for _, weight, _ in steps)
            done = 0.0
            for title, weight, action in steps:
                if self.stop.is_set():
                    raise Stopped()
                self.note("== " + title)
                self.report("step", title)
                self.report("progress", done / total)
                action(lambda part, done=done, weight=weight: self.report("progress", (done + weight * min(1.0, part)) / total))
                done += weight
            self.report("progress", 1.0)
            self.report("done", self.launch)
        except Stopped:
            self.report("stopped", "")
        except Failure as failure:
            self.note("failed: " + str(failure))
            self.report("failed", (title, str(failure)))
        except Exception as error:
            if self.stop.is_set():
                self.report("stopped", "")
            else:
                self.note("failed: " + repr(error))
                self.report("failed", (title, str(error) or type(error).__name__))
        finally:
            if self.log:
                self.log.close()

    def note(self, line):
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()

    def program(self, name):
        for tool in TOOLS:
            candidate = self.tools / Path(tool["path"]) / (name if UNIX else name + ".exe")
            if candidate.is_file():
                return str(candidate)
        return name

    def run(self, arguments, line_seen=None):
        self.note("> " + " ".join(arguments))
        process = subprocess.Popen(arguments, cwd=self.root, env=self.environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=0 if UNIX else HIDDEN, encoding="utf-8", errors="replace")
        self.process = process
        tail = collections.deque(maxlen=8)
        output = []
        for line in process.stdout:
            line = line.rstrip()
            if not line:
                continue
            self.note(line)
            tail.append(line[:300])
            output.append(line)
            if line_seen:
                line_seen(line)
        code = process.wait()
        self.process = None
        if self.stop.is_set():
            raise Stopped()
        if code != 0:
            raise Failure("\n".join(tail) or f"{Path(arguments[0]).name}: {code}")
        return "\n".join(output)

    def copy_sources(self, move):
        with zipfile.ZipFile(self.bundle / SOURCES) as archive:
            names = archive.namelist()
            for index, name in enumerate(names):
                if self.stop.is_set():
                    raise Stopped()
                archive.extract(name, self.root)
                if index % 20 == 0:
                    move(index / len(names))
        sdl = self.bundle / SDL_BUNDLE
        if LINUX and sdl.is_dir() and sdl.resolve() != (self.root / SDL_BUNDLE).resolve():
            shutil.copytree(sdl, self.root / SDL_BUNDLE, dirs_exist_ok=True)

    def tool_ready(self, tool):
        stamp = self.tools / tool["folder"] / ".ready"
        try:
            return stamp.read_text() == tool["sha256"]
        except OSError:
            return False

    def digest(self, path):
        value = hashlib.sha256()
        with open(path, "rb") as file:
            while block := file.read(1 << 20):
                if self.stop.is_set():
                    raise Stopped()
                value.update(block)
        return value.hexdigest()

    def download(self, tool, move):
        folder = self.root / "downloads"
        folder.mkdir(parents=True, exist_ok=True)
        target = folder / tool["file"]
        if target.is_file() and self.digest(target) == tool["sha256"]:
            return
        partial = folder / (tool["file"] + ".part")
        reason = ""
        for attempt in range(3):
            try:
                request = urllib.request.Request(tool["url"], headers={"User-Agent": "WondersBeyondRecomp-Setup"})
                with urllib.request.urlopen(request, timeout=30) as response, open(partial, "wb") as file:
                    size = int(response.headers.get("Content-Length") or tool["megabytes"] << 20)
                    received = 0
                    while block := response.read(1 << 16):
                        if self.stop.is_set():
                            raise Stopped()
                        file.write(block)
                        received += len(block)
                        move(received / size)
                break
            except OSError as error:
                reason = str(error)
                self.note(f"download attempt {attempt + 1}: {reason}")
        else:
            raise Failure(self.texts["download_failed"].format(name=tool["name"], reason=reason))
        if self.digest(partial) != tool["sha256"]:
            partial.unlink(missing_ok=True)
            raise Failure(self.texts["checksum"].format(name=tool["name"]))
        os.replace(partial, target)

    def unpack(self, tool):
        archive = self.root / "downloads" / tool["file"]
        folder = self.tools / tool["folder"]
        shutil.rmtree(folder, ignore_errors=True)
        folder.mkdir(parents=True, exist_ok=True)
        if tool["kind"] == "single":
            shutil.copyfile(archive, folder / tool["file"])
        elif tool["kind"] == "zip":
            with zipfile.ZipFile(archive) as content:
                content.extractall(folder)
        elif UNIX:
            self.run(["tar", "-xf", str(archive), "-C", str(folder)])
        else:
            tar = Path(os.environ.get("SystemRoot", "C:\\Windows")) / "System32" / "tar.exe"
            try:
                self.run([str(tar), "-xf", str(archive), "-C", str(folder)])
            except Failure:
                if archive.suffix.lower() != ".exe":
                    raise
                self.note("tar could not unpack the archive, letting it unpack itself")
                shutil.rmtree(folder, ignore_errors=True)
                folder.mkdir(parents=True, exist_ok=True)
                self.run([str(archive), "-y", f"-o{folder}"])
        if self.stop.is_set():
            raise Stopped()
        if UNIX and tool["kind"] != "tar":
            for file in folder.rglob("*"):
                if file.is_file():
                    file.chmod(0o755)
        (folder / ".ready").write_text(tool["sha256"])
        archive.unlink(missing_ok=True)

    def verify_disc(self, move):
        output = self.run([self.program("nodtool"), "--no-color", "verify", self.disc])
        if any("\u274c" in line and not line.startswith("Redump:") for line in output.splitlines()):
            raise Failure(self.texts["disc_damaged"])

    def extract_disc(self, move):
        target = self.root / "game" / "extracted"
        shutil.rmtree(target, ignore_errors=True)
        try:
            self.run([self.program("nodtool"), "--no-color", "extract", self.disc, "game/extracted"])
        except (Failure, Stopped):
            shutil.rmtree(target, ignore_errors=True)
            raise

    def identify_disc(self, move):
        folder = self.root / "game" / "extracted"
        try:
            with open(folder / "sys" / "boot.bin", "rb") as file:
                found = file.read(6).decode("ascii", "replace")
        except OSError as error:
            raise Failure(str(error))
        if found != DISC_ID:
            shutil.rmtree(folder, ignore_errors=True)
            raise Failure(self.texts["disc_other"].format(found=found, wanted=DISC_ID))

    def configure(self, move):
        if built_elsewhere(self.root):
            self.note("build/out was configured in another folder, configuring again")
            shutil.rmtree(self.root / "build" / "out", ignore_errors=True)
        command = [self.program("cmake"), "-S", ".", "-B", "build/out", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
        if LINUX:
            command += self.linux_options()
        self.run(command)

    def linux_options(self):
        zig = self.tools / Path(next(tool["path"] for tool in TOOLS if tool["folder"] == "zig")) / "zig"
        wrappers = self.root / "build" / "zig"
        wrappers.mkdir(parents=True, exist_ok=True)
        options = []
        for name, variable in (("cc", "CMAKE_C_COMPILER"), ("c++", "CMAKE_CXX_COMPILER"), ("ar", "CMAKE_AR"), ("ranlib", "CMAKE_RANLIB")):
            wrapper = wrappers / name
            wrapper.write_text("#!/bin/sh" + chr(10) + f'exec "{zig}" {name} "$@"' + chr(10))
            wrapper.chmod(0o755)
            options.append(f"-D{variable}={wrapper}")
        self.environment["ZIG_GLOBAL_CACHE_DIR"] = str(self.root / "build" / "zig-cache")
        self.environment["ZIG_LOCAL_CACHE_DIR"] = str(self.root / "build" / "zig-cache")
        sdl = self.root / SDL_BUNDLE
        return options + [f"-DSDL3_DIR={sdl / 'lib' / 'cmake' / 'SDL3'}", "-DWP_FETCH_SDL3=OFF", "-DWP_REQUIRE_SDL3=ON",
                          "-DCMAKE_BUILD_RPATH=$ORIGIN", "-DCMAKE_INSTALL_RPATH=$ORIGIN"]

    def build(self, move):
        pattern = re.compile(r"^\[(\d+)/(\d+)\]")

        def line_seen(line):
            match = pattern.match(line)
            if match and int(match.group(2)):
                move(int(match.group(1)) / int(match.group(2)))

        self.run([self.program("cmake"), "--build", "build/out", "--parallel", str(compile_jobs()), "--target", GAME], line_seen)

    def finish(self, move):
        built = self.root / "build" / "out"
        if not (built / EXECUTABLE).is_file():
            raise Failure(EXECUTABLE)
        for file in [built / EXECUTABLE] + sorted(built.glob("*.dll")):
            shutil.copyfile(file, self.root / file.name)
        if LINUX:
            (self.root / EXECUTABLE).chmod(0o755)
            for library in sorted((self.root / SDL_BUNDLE / "lib").glob("libSDL3.so*")):
                shutil.copyfile(library, self.root / library.name)
        if not (self.root / "python").exists():
            shutil.copytree(Path(self.python).parent, self.root / "python")
        (self.root / BUILT_FROM).write_text(sources_digest(self.bundle))
        shutil.rmtree(self.root / "downloads", ignore_errors=True)
        if MAC:
            return self.make_app()
        if LINUX:
            return self.make_desktop_entry()
        links = [shell_folder(0x02)] if self.menu else []
        if self.shortcut:
            links.append(shell_folder(0x10))
        for folder in links:
            try:
                create_shortcut(folder / (SHORTCUT_NAME + ".lnk"), self.root / EXECUTABLE, self.root)
            except OSError as error:
                self.note(f"shortcut in {folder}: {error}")

    def make_desktop_entry(self):
        icon = self.root / "game" / "res" / (GAME + ".png")
        entry = chr(10).join([
            "[Desktop Entry]",
            "Type=Application",
            f"Name={SHORTCUT_NAME}",
            "Comment=PokePark 2: Wonders Beyond, rebuilt for this computer",
            f'Exec=env WP_ROOT="{self.root}" "{self.root / EXECUTABLE}"',
            f"Path={self.root}",
            f"Icon={icon}",
            "Terminal=false",
            "Categories=Game;",
            "",
        ])
        folders = [Path.home() / ".local" / "share" / "applications"] if self.menu else []
        if self.shortcut:
            folders.append(Path.home() / "Desktop")
        for folder in folders:
            try:
                folder.mkdir(parents=True, exist_ok=True)
                target = folder / (APP_IDENTIFIER + ".desktop")
                target.write_text(entry, encoding="utf-8")
                target.chmod(0o755)
            except OSError as error:
                self.note(f"desktop entry in {folder}: {error}")

    def make_app(self):
        app = Path(self.launch)
        shutil.rmtree(app, ignore_errors=True)
        contents = app / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "Resources").mkdir()
        executable = contents / "MacOS" / GAME
        shutil.copyfile(self.root / EXECUTABLE, executable)
        executable.chmod(0o755)
        self.make_icon(contents / "Resources" / "icon.icns")
        (contents / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleExecutable": GAME,
            "CFBundleIdentifier": APP_IDENTIFIER,
            "CFBundleName": SHORTCUT_NAME,
            "CFBundleDisplayName": SHORTCUT_NAME,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "1.0",
            "CFBundleIconFile": "icon",
            "LSMinimumSystemVersion": "11.0",
            "NSHighResolutionCapable": True,
            "LSEnvironment": {"WP_ROOT": str(self.root)},
        }))
        register = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
        for arguments in (["codesign", "--force", "--sign", "-", str(app)], [register, "-f", str(app)]):
            try:
                self.run(arguments)
            except (Failure, OSError) as error:
                self.note(f"{arguments[0]}: {error}")
        if self.shortcut:
            link = Path.home() / "Desktop" / app.name
            try:
                if link.is_symlink() or link.exists():
                    link.unlink()
                link.symlink_to(app)
            except OSError as error:
                self.note(f"desktop link: {error}")

    def make_icon(self, target):
        source = self.root / "game" / "res" / (GAME + ".png")
        icons = self.root / "build" / "app.iconset"
        shutil.rmtree(icons, ignore_errors=True)
        icons.mkdir(parents=True)
        sizes = [(16, "16x16"), (32, "16x16@2x"), (32, "32x32"), (64, "32x32@2x"), (128, "128x128"), (256, "128x128@2x"), (256, "256x256")]
        try:
            for size, name in sizes:
                self.run(["sips", "-z", str(size), str(size), str(source), "--out", str(icons / f"icon_{name}.png")])
            self.run(["iconutil", "-c", "icns", str(icons), "-o", str(target)])
        except (Failure, OSError) as error:
            self.note(f"icon: {error}")
