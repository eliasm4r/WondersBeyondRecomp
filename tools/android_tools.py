import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "toolchain" / "android"
GOOGLE = "https://dl.google.com/android/repository/"
WINDOWS = sys.platform == "win32"

PACKAGES = {
    "win32": [
        {"name": "Android NDK r30", "url": GOOGLE + "android-ndk-r30-windows.zip", "sha1": "9bf167a1985fa7d4a036186b78f702eab9179408", "folder": "ndk"},
        {"name": "Android build tools 36.1", "url": GOOGLE + "build-tools_r36.1_windows.zip", "sha1": "95f048ae758dad90617975fe3f787f038b03338c",
         "folder": "sdk/build-tools/36.1.0"},
        {"name": "Android 16 platform", "url": GOOGLE + "platform-36_r02.zip", "sha1": "2c1a80dd4d9f7d0e6dd336ec603d9b5c55a6f576", "folder": "sdk/platforms/android-36"},
        {"name": "Temurin JDK 17", "url": "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.21%2B9/OpenJDK17U-jdk_x64_windows_hotspot_17.0.21_9.zip",
         "sha256": "98518ecf554465057ee5509462631deeee374bc368c12a411d225cd29381f84b", "folder": "jdk"},
    ],
    "linux": [
        {"name": "Android NDK r30", "url": GOOGLE + "android-ndk-r30-linux.zip", "sha1": "5107f898313790e449e87eee2183d9a20602dee9", "folder": "ndk"},
        {"name": "Android build tools 36.1", "url": GOOGLE + "build-tools_r36.1_linux.zip", "sha1": "936a0d6bd5ae3e2118a7567dddbc95ff67ed46e9",
         "folder": "sdk/build-tools/36.1.0"},
        {"name": "Android 16 platform", "url": GOOGLE + "platform-36_r02.zip", "sha1": "2c1a80dd4d9f7d0e6dd336ec603d9b5c55a6f576", "folder": "sdk/platforms/android-36"},
    ],
}

NOTICE = ("The Android tools come from Google (Android Software Development Kit License Agreement, https://developer.android.com/studio/terms) "
          "and Eclipse Adoptium (GPL-2.0 with the Classpath Exception). Downloading them means accepting those terms.")


def digest(path, kind):
    value = hashlib.new(kind)
    with open(path, "rb") as file:
        while block := file.read(1 << 20):
            value.update(block)
    return value.hexdigest()


def archive_name(package):
    return package["url"].rsplit("/", 1)[1].replace("%2B", "+")


def fetch(package):
    downloads = TOOLS / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    target = downloads / archive_name(package)
    kind = "sha256" if "sha256" in package else "sha1"
    if target.is_file() and digest(target, kind) == package[kind]:
        return target
    print(f"downloading {package['name']}", flush=True)
    partial = target.with_name(target.name + ".part")
    request = urllib.request.Request(package["url"], headers={"User-Agent": "WondersBeyondRecomp-Android"})
    with urllib.request.urlopen(request, timeout=60) as response, open(partial, "wb") as file:
        shutil.copyfileobj(response, file, 1 << 20)
    if digest(partial, kind) != package[kind]:
        partial.unlink()
        sys.exit(f"{package['name']} did not download correctly, try again")
    os.replace(partial, target)
    return target


def unpack(package, archive):
    folder = TOOLS / package["folder"]
    stamp = folder / ".ready"
    if stamp.is_file() and stamp.read_text() == archive.name:
        return folder
    print(f"unpacking {package['name']}", flush=True)
    shutil.rmtree(folder, ignore_errors=True)
    staging = TOOLS / "staging"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    with zipfile.ZipFile(archive) as content:
        for entry in content.infolist():
            path = content.extract(entry, staging)
            mode = entry.external_attr >> 16
            if not WINDOWS and mode & 0o111:
                os.chmod(path, mode & 0o777)
    entries = list(staging.iterdir())
    top = entries[0] if len(entries) == 1 and entries[0].is_dir() else staging
    folder.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(top), str(folder))
    shutil.rmtree(staging, ignore_errors=True)
    stamp.write_text(archive.name)
    return folder


def main():
    parser = argparse.ArgumentParser(description="Download the Android tools and build the Android package of the game")
    parser.add_argument("--only-tools", action="store_true")
    parser.add_argument("--target", default="pokepark2")
    parser.add_argument("--abi", default="arm64-v8a")
    options = parser.parse_args()
    packages = PACKAGES.get("win32" if WINDOWS else sys.platform if sys.platform == "linux" else "")
    if not packages:
        sys.exit("the Android package is built on Windows or Linux")
    print(NOTICE, flush=True)
    for package in packages:
        stamp = TOOLS / package["folder"] / ".ready"
        if not (stamp.is_file() and stamp.read_text() == archive_name(package)):
            unpack(package, fetch(package))
    shutil.rmtree(TOOLS / "downloads", ignore_errors=True)
    if options.only_tools:
        return 0
    java = TOOLS / "jdk" if (TOOLS / "jdk").is_dir() else Path(os.environ.get("JAVA_HOME", ""))
    cmake = shutil.which("cmake") or "cmake"
    command = [sys.executable, str(ROOT / "tools" / "build_apk.py"), "--sdk", str(TOOLS / "sdk"), "--ndk", str(TOOLS / "ndk"), "--java", str(java),
               "--cmake", cmake, "--target", options.target, "--abi", options.abi]
    return subprocess.call(command, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
