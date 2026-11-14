import collections
import ctypes
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

GAME = "pokepark2"
DISC_ID = "S2LP01"
OLD_FOLDER = Path("games") / GAME
FOLDER_NAME = "WondersBeyondRecomp"
EXECUTABLE = "pokepark2.exe"
SHORTCUT_NAME = "PokePark 2 Wonders Beyond"
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

HIDDEN = 0x08000000


class Stopped(Exception):
    pass


class Failure(Exception):
    pass


def default_folder():
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
        self.python = str(self.bundle / "python" / "python.exe")
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
            self.report("done", str(self.root / EXECUTABLE))
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
            candidate = self.tools / Path(tool["path"]) / (name + ".exe")
            if candidate.is_file():
                return str(candidate)
        return name

    def run(self, arguments, line_seen=None):
        self.note("> " + " ".join(arguments))
        process = subprocess.Popen(arguments, cwd=self.root, env=self.environment, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=HIDDEN, encoding="utf-8", errors="replace")
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
        else:
            tar = Path(os.environ.get("SystemRoot", "C:\\Windows")) / "System32" / "tar.exe"
            self.run([str(tar), "-xf", str(archive), "-C", str(folder)])
        if self.stop.is_set():
            raise Stopped()
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
        except OSError:
            found = "?"
        if found != DISC_ID:
            shutil.rmtree(folder, ignore_errors=True)
            raise Failure(self.texts["disc_other"].format(found=found, wanted=DISC_ID))

    def configure(self, move):
        if built_elsewhere(self.root):
            self.note("build/out was configured in another folder, configuring again")
            shutil.rmtree(self.root / "build" / "out", ignore_errors=True)
        self.run([self.program("cmake"), "-S", ".", "-B", "build/out", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])

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
        (self.root / BUILT_FROM).write_text(sources_digest(self.bundle))
        shutil.rmtree(self.root / "downloads", ignore_errors=True)
        links = [shell_folder(0x02)] if self.menu else []
        if self.shortcut:
            links.append(shell_folder(0x10))
        for folder in links:
            try:
                create_shortcut(folder / (SHORTCUT_NAME + ".lnk"), self.root / EXECUTABLE, self.root)
            except OSError as error:
                self.note(f"shortcut in {folder}: {error}")
