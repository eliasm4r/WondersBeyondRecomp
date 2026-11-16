import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = "io.github.eliasm4r.wondersbeyondrecomp"
MIN_SDK = 26
KEY_ALIAS = "wondersbeyond"
KEY_PASSWORD = "wondersbeyond"
WINDOWS = sys.platform == "win32"
OUTPUTS = {"pokepark2": "apk", "android_smoke": "smoke"}


def newest(folder, pattern):
    found = sorted(Path(folder).glob(pattern), key=lambda path: [int(part) if part.isdigit() else part for part in re.split(r"[.\-_]", path.name)])
    if not found:
        sys.exit(f"nothing matches {pattern} in {folder}")
    return found[-1]


def tool(folder, name):
    for suffix in ((".exe", ".bat", "") if WINDOWS else ("",)):
        candidate = Path(folder) / (name + suffix)
        if candidate.is_file():
            return str(candidate)
    sys.exit(f"{name} is missing from {folder}")


def run(arguments, **options):
    print("> " + " ".join(str(part) for part in arguments), flush=True)
    subprocess.run([str(part) for part in arguments], check=True, **options)


def project_value(name):
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r'set\(' + name + r' "([^"]+)"', text)
    return match.group(1) if match else "0"


def version_code():
    digits = re.sub(r"\D", "", project_value("WP_BUILD"))
    return int(digits or "1")


def build_library(options, build):
    toolchain = Path(options.ndk) / "build" / "cmake" / "android.toolchain.cmake"
    if not (build / "CMakeCache.txt").is_file():
        run([options.cmake, "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
             f"-DANDROID_ABI={options.abi}", f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DWP_REQUIRE_SDL3=ON"])
    command = [options.cmake, "--build", build, "--target", options.target]
    if options.jobs:
        command += ["--parallel", str(options.jobs)]
    run(command)
    library = build / OUTPUTS[options.target] / "libmain.so"
    if not library.is_file():
        sys.exit(f"{library} was not built")
    return library


def strip(options, library, target):
    host = "windows-x86_64" if WINDOWS else ("darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64")
    stripper = Path(options.ndk) / "toolchains" / "llvm" / "prebuilt" / host / "bin" / ("llvm-strip.exe" if WINDOWS else "llvm-strip")
    target.parent.mkdir(parents=True, exist_ok=True)
    if stripper.is_file():
        run([stripper, "--strip-unneeded", "-o", target, library])
    else:
        shutil.copyfile(library, target)


def compile_java(options, build, work, android_jar):
    sdl_java = build / "_deps" / "sdl3-src" / "android-project" / "app" / "src" / "main" / "java"
    if not sdl_java.is_dir():
        sys.exit(f"the SDL3 Java sources are missing from {sdl_java}")
    sources = sorted(str(path) for folder in (sdl_java, ROOT / "android" / "java") for path in Path(folder).rglob("*.java"))
    classes = work / "classes"
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True)
    javac = tool(Path(options.java) / "bin", "javac")
    run([javac, "-nowarn", "-encoding", "UTF-8", "-source", "8", "-target", "8", "-bootclasspath", android_jar, "-d", classes] + sources)
    dex = work / "dex"
    shutil.rmtree(dex, ignore_errors=True)
    dex.mkdir()
    run([options.d8, "--release", "--min-api", str(MIN_SDK), "--lib", android_jar, "--output", dex] + sorted(str(path) for path in classes.rglob("*.class")),
        env=options.environment)
    return dex / "classes.dex"


def link_resources(options, work, android_jar):
    resources = work / "res"
    shutil.rmtree(resources, ignore_errors=True)
    icon = resources / "mipmap" / "ic_launcher.png"
    icon.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "game" / "res" / "pokepark2.png", icon)
    compiled = work / "resources.zip"
    run([options.aapt2, "compile", "--dir", resources, "-o", compiled])
    base = work / "base.apk"
    run([options.aapt2, "link", "-o", base, "-I", android_jar, "--manifest", ROOT / "android" / "AndroidManifest.xml", "--min-sdk-version", str(MIN_SDK),
         "--target-sdk-version", str(options.target_sdk), "--version-code", str(version_code()), "--version-name", project_value("WP_VERSION"), compiled])
    return base


def ensure_key(options):
    keystore = Path(options.keystore)
    if keystore.is_file():
        return keystore
    keystore.parent.mkdir(parents=True, exist_ok=True)
    keytool = tool(Path(options.java) / "bin", "keytool")
    run([keytool, "-genkeypair", "-keystore", keystore, "-storepass", KEY_PASSWORD, "-keypass", KEY_PASSWORD, "-alias", KEY_ALIAS, "-keyalg", "RSA",
         "-keysize", "2048", "-validity", "36500", "-dname", "CN=PokePark 2 Wonders Beyond Recomp"])
    return keystore


def main():
    parser = argparse.ArgumentParser(description="Build the Android package from the translated game code")
    parser.add_argument("--sdk", default=os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT"))
    parser.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_LATEST_HOME"))
    parser.add_argument("--java", default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--target", default="pokepark2", choices=sorted(OUTPUTS))
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--target-sdk", type=int, default=35)
    parser.add_argument("--keystore", default=str(ROOT / "game" / "android.keystore"))
    parser.add_argument("--output", default=str(ROOT / "build" / "dist" / "WondersBeyondRecomp.apk"))
    options = parser.parse_args()
    for name in ("sdk", "ndk", "java"):
        if not getattr(options, name) or not Path(getattr(options, name)).is_dir():
            sys.exit(f"--{name} must name an existing folder")
    options.environment = dict(os.environ, JAVA_HOME=options.java)
    options.environment["PATH"] = str(Path(options.java) / "bin") + os.pathsep + options.environment.get("PATH", "")
    build_tools = newest(Path(options.sdk) / "build-tools", "*")
    android_jar = str(newest(Path(options.sdk) / "platforms", "android-*") / "android.jar")
    options.aapt2 = tool(build_tools, "aapt2")
    options.d8 = tool(build_tools, "d8")
    zipalign = tool(build_tools, "zipalign")
    apksigner = tool(build_tools, "apksigner")

    build = ROOT / "build" / f"android-{options.abi}"
    work = build / "package"
    library = build_library(options, build)
    stripped = work / "lib" / options.abi / "libmain.so"
    strip(options, library, stripped)
    dex = compile_java(options, build, work, android_jar)
    base = link_resources(options, work, android_jar)

    unaligned = work / "unaligned.apk"
    shutil.copyfile(base, unaligned)
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as archive:
        archive.write(dex, "classes.dex")
        archive.write(stripped, f"lib/{options.abi}/libmain.so")
    aligned = work / "aligned.apk"
    run([zipalign, "-p", "-f", "4", unaligned, aligned])
    keystore = ensure_key(options)
    output = Path(options.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    run([apksigner, "sign", "--ks", keystore, "--ks-pass", f"pass:{KEY_PASSWORD}", "--ks-key-alias", KEY_ALIAS, "--key-pass", f"pass:{KEY_PASSWORD}",
         "--out", output, aligned], env=options.environment)
    run([apksigner, "verify", output], env=options.environment)
    print(f"{output} {output.stat().st_size / (1 << 20):.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
