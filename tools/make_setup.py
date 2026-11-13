import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

PYTHON_URL = "https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg"
PYTHON_SHA256 = "0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8"
TCLTK_URL = "https://www.python.org/ftp/python/3.12.10/amd64/tcltk.msi"
TCLTK_SHA256 = "55c96ffad69b1c834aa52e11b9ce41637a178ba6ad6607e83956044834276e2a"
SOURCES = ["CMakeLists.txt", "LICENSE", "THIRD_PARTY_NOTICES.md", "engine", "recompiler", "game", "third_party", "tests", "tools/fetch_sdl.py"]
UNUSED_LIBRARIES = ["site-packages", "test", "idlelib", "ensurepip", "turtledemo", "venv", "lib2to3", "__phello__"]
NAME = "WondersBeyondRecomp-Setup"
SIGNED = "Setup (if blocked)"

root = Path(__file__).resolve().parent.parent
work = root / "build" / "setup"
bundle = work / NAME


def fetch(url, sha256):
    target = work / "cache" / url.rsplit("/", 1)[1]
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.is_file():
        print(f"downloading {url}")
        request = urllib.request.Request(url, headers={"User-Agent": NAME})
        with urllib.request.urlopen(request) as response, open(target, "wb") as file:
            shutil.copyfileobj(response, file)
    if hashlib.sha256(target.read_bytes()).hexdigest() != sha256:
        target.unlink()
        sys.exit(f"{target.name} does not match its checksum")
    return target


def add_python():
    package = fetch(PYTHON_URL, PYTHON_SHA256)
    python = bundle / "python"
    with zipfile.ZipFile(package) as archive:
        for name in archive.namelist():
            parts = Path(name).parts
            if len(parts) < 2 or parts[0] != "tools" or parts[1] in ("include", "libs") or name.endswith("/"):
                continue
            if parts[1] == "Lib" and len(parts) > 2 and parts[2] in UNUSED_LIBRARIES:
                continue
            target = python.joinpath(*parts[1:])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.read(name))
    installer = fetch(TCLTK_URL, TCLTK_SHA256)
    unpacked = work / "tcltk"
    shutil.rmtree(unpacked, ignore_errors=True)
    subprocess.run(f'msiexec /a "{installer}" /qn TARGETDIR="{unpacked}"', check=True)
    for file in (unpacked / "DLLs").iterdir():
        shutil.copyfile(file, python / "DLLs" / file.name)
    shutil.copytree(unpacked / "Lib" / "tkinter", python / "Lib" / "tkinter")
    for folder in ("tcl8", "tcl8.6", "tk8.6"):
        shutil.copytree(unpacked / "tcl" / folder, python / "tcl" / folder, ignore=shutil.ignore_patterns("demos"))


def check_signatures():
    command = ("Get-ChildItem -LiteralPath $env:WP_BUNDLE -Recurse -File | "
               "Where-Object { $_.Extension -in '.exe','.dll','.pyd' -and (Get-AuthenticodeSignature -LiteralPath $_.FullName).Status -ne 'Valid' } | ForEach-Object { $_.FullName }")
    result = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True, env={**os.environ, "WP_BUNDLE": str(bundle)})
    unsigned = result.stdout.strip()
    if result.returncode != 0 or unsigned:
        sys.exit("programs without a valid signature in the setup:\n" + (unsigned or result.stderr))


def add_launcher():
    build = work / "launcher"
    shutil.rmtree(build, ignore_errors=True)
    build.mkdir(parents=True)
    shutil.copyfile(root / "game" / "res" / "pokepark2.ico", build / "game.ico")
    (build / "launcher.rc").write_text('1 ICON "game.ico"\n', encoding="ascii")
    subprocess.run(["windres", "launcher.rc", "-O", "coff", "-o", "launcher.res"], cwd=build, check=True)
    subprocess.run(["gcc", "-Os", "-s", "-mwindows", "-municode", str(root / "installer" / "launcher.c"), "launcher.res", "-o", str(bundle / "Setup.exe")],
                   cwd=build, check=True)


def add_sources():
    listed = subprocess.run(["git", "ls-files", "-z", "--"] + SOURCES, cwd=root, capture_output=True, check=True).stdout.decode("utf-8")
    with zipfile.ZipFile(bundle / "installer" / "sources.zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(filter(None, listed.split("\0"))):
            if (root / name).is_file():
                archive.write(root / name, name)


def main():
    if sys.platform != "win32":
        sys.exit("the setup is made on Windows")
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir(parents=True)
    add_python()
    scripts = bundle / "installer"
    scripts.mkdir()
    add_sources()
    for file in sorted((root / "installer").glob("*.py")):
        shutil.copyfile(file, scripts / file.name)
    shutil.copyfile(root / "game" / "res" / "pokepark2.ico", scripts / "game.ico")
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copyfile(root / name, bundle / name)
    shutil.copyfile(bundle / "python" / "pythonw.exe", bundle / "python" / (SIGNED + ".exe"))
    (bundle / "python" / (SIGNED + "._pth")).write_bytes(b"Lib\r\nDLLs\r\n..\\installer\r\nimport site\r\n")
    check_signatures()
    add_launcher()
    output = root / "build" / "dist" / (NAME + "-Windows.zip")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for file in sorted(bundle.rglob("*")):
            if file.is_file():
                archive.write(file, Path(NAME) / file.relative_to(bundle))
    print(f"{output} {output.stat().st_size / (1 << 20):.1f} MB")


if __name__ == "__main__":
    sys.exit(main())
