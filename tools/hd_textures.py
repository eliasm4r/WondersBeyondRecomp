import argparse
import hashlib
import re
import shutil
import subprocess
import struct
import sys
import time
import urllib.request
import zipfile
import zlib
from pathlib import Path

UPSCALER = {
    "name": "Real-ESRGAN ncnn Vulkan 0.2.5.0",
    "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip",
    "sha256": "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d",
}
MODELS = {
    ("anime", 4): "realesr-animevideov3",
    ("anime", 2): "realesr-animevideov3",
    ("general", 4): "realesrgan-x4plus",
    ("drawn", 4): "realesrgan-x4plus-anime",
}
NAME = re.compile(r"^tex1_(\d+)x(\d+)(_m)?_[0-9a-f]{16}(_[0-9a-f]{16}|_\$)?_(\d+)$")
SMALLEST = 8
PADDED_SIDE = 16
TILE = 200
INTENSITY = ("0", "1")
GRAY = ("0", "1", "2", "3")
ALPHA_IS_GRAY = ("0", "1")
WORST_GRAY_ERROR = 12
WORST_VISIBLE_GRAY_ERROR = 28
VIDEO_FRAMES = 16
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}
HIDDEN = 0x08000000 if sys.platform == "win32" else 0


def stems(folder):
    found = set()
    if folder.is_dir():
        for file in folder.rglob("*.png"):
            stem = file.stem.lower()
            found.add(stem[:-4] if stem.endswith("_arb") else stem)
    return found


def png_rows(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    at = 8
    header = None
    packed = []
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            packed.append(body)
        elif kind == b"IEND":
            break
        at += 12 + length
    if not header or header[2] != 8 or header[3] not in CHANNELS or header[6] != 0:
        raise ValueError("unsupported PNG layout")
    width, height, channels = header[0], header[1], CHANNELS[header[3]]
    return width, height, channels, zlib.decompress(b"".join(packed))


def is_blank(path):
    try:
        width, height, channels, raw = png_rows(path)
    except (OSError, ValueError, zlib.error):
        return True
    stride = width * channels + 1
    return not any(raw[row * stride + 1:(row + 1) * stride].strip(b"\0") for row in range(height))


def read_png(path):
    width, height, channels, raw = png_rows(path)
    stride = width * channels
    pixels = bytearray(stride * height)
    previous = bytearray(stride)
    for row in range(height):
        kind = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 255
        elif kind == 3:
            for i in range(stride):
                line[i] = (line[i] + ((line[i - channels] if i >= channels else 0) + previous[i]) // 2) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                guess = a + b - c
                pa, pb, pc = abs(guess - a), abs(guess - b), abs(guess - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        pixels[row * stride:(row + 1) * stride] = line
        previous = line
    return width, height, channels, pixels


def write_png(path, width, height, channels, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    kind = next(key for key, value in CHANNELS.items() if value == channels)
    header = struct.pack(">IIBBBBB", width, height, 8, kind, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def texture_format(name):
    match = NAME.match(Path(name).stem.lower())
    return match.group(5) if match else None


def as_rgba(channels, pixels):
    if channels == 4:
        return bytearray(pixels)
    count = len(pixels) // channels
    out = bytearray(count * 4)
    if channels == 3:
        out[0::4], out[1::4], out[2::4] = pixels[0::3], pixels[1::3], pixels[2::3]
        out[3::4] = b"\xff" * count
    elif channels == 2:
        out[0::4] = out[1::4] = out[2::4] = pixels[0::2]
        out[3::4] = pixels[1::2]
    else:
        out[0::4] = out[1::4] = out[2::4] = pixels
        out[3::4] = b"\xff" * count
    return out


def save(path, width, height, channels, pixels):
    stride = width * channels
    write_png(path, width, height, channels, b"".join(b"\0" + pixels[row * stride:(row + 1) * stride] for row in range(height)))


def safe_side(side):
    rest = side % TILE
    return side if rest == 0 or rest >= PADDED_SIDE else side + PADDED_SIDE - rest


def pad(pixels, width, height):
    wide = safe_side(width)
    tall = safe_side(height)
    rows = []
    for row in range(height):
        line = pixels[row * width * 4:(row + 1) * width * 4]
        rows.append(line + line[-4:] * (wide - width))
    rows += [rows[-1]] * (tall - height)
    return bytearray(b"".join(rows)), wide, tall


def crop(pixels, stride, width, height):
    return bytearray(b"".join(pixels[row * stride * 4:row * stride * 4 + width * 4] for row in range(height)))


def prepare(file, folder):
    width, height, channels, pixels = read_png(file)
    if not pixels.strip(b"\0"):
        return False
    pixels = as_rgba(channels, pixels)
    kind = texture_format(file.name)
    if kind in GRAY:
        pixels[1::4] = pixels[2::4] = pixels[0::4]
    if safe_side(width) != width or safe_side(height) != height:
        pixels, width, height = pad(pixels, width, height)
    if kind in ALPHA_IS_GRAY or not pixels[3::4].strip(b"\xff"):
        rgb = bytearray(width * height * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = pixels[0::4], pixels[1::4], pixels[2::4]
        save(folder / file.name, width, height, 3, rgb)
    else:
        save(folder / file.name, width, height, 4, pixels)
    return True


def gray_error(level, width, original, alpha, small_width, small_height):
    scale = width // small_width
    total = 0
    weighted = 0
    weight = 0
    for row in range(small_height):
        top = level[(row * scale + scale // 2 - 1) * width:(row * scale + scale // 2) * width]
        bottom = level[(row * scale + scale // 2) * width:(row * scale + scale // 2 + 1) * width]
        line = original[row * small_width:(row + 1) * small_width]
        seen = alpha[row * small_width:(row + 1) * small_width]
        for column, value in enumerate(line):
            x = column * scale + scale // 2
            difference = abs((top[x - 1] + top[x] + bottom[x - 1] + bottom[x]) // 4 - value)
            total += difference
            weighted += difference * seen[column]
            weight += seen[column]
    return total / (small_width * small_height), weighted / weight if weight else 0


def finish(source, larger):
    if not larger.is_file() or is_blank(larger):
        return "empty"
    kind = texture_format(larger.name)
    match = NAME.match(larger.stem.lower())
    true_width, true_height = (int(match.group(1)), int(match.group(2))) if match else (0, 0)
    padded = match is not None and (safe_side(true_width) != true_width or safe_side(true_height) != true_height)
    if kind not in GRAY and not padded:
        return "ok"
    try:
        width, height, channels, pixels = read_png(larger)
        small_width, small_height, small_channels, original = read_png(source)
    except (OSError, ValueError, zlib.error):
        return "empty"
    pixels = as_rgba(channels, pixels)
    original = as_rgba(small_channels, original)
    if padded:
        scale = width // small_width
        pixels = crop(pixels, width, true_width * scale, true_height * scale)
        width, height = true_width * scale, true_height * scale
        original = crop(original, small_width, true_width, true_height)
        small_width, small_height = true_width, true_height
    if kind in GRAY:
        level = bytes((red + green + blue) // 3 for red, green, blue in zip(pixels[0::4], pixels[1::4], pixels[2::4]))
        seen = original[0::4] if kind in ALPHA_IS_GRAY else original[3::4]
        plain, visible = gray_error(level, width, original[0::4], seen, small_width, small_height)
        if plain > WORST_GRAY_ERROR or visible > WORST_VISIBLE_GRAY_ERROR:
            larger.unlink()
            return "kept"
        pixels[0::4] = pixels[1::4] = pixels[2::4] = level
        if kind in ALPHA_IS_GRAY:
            pixels[3::4] = level
    save(larger, width, height, 4, pixels)
    return "ok"


def upscaler_ready(folder):
    try:
        return (folder / ".ready").read_text() == UPSCALER["sha256"] and (folder / "realesrgan-ncnn-vulkan.exe").is_file()
    except OSError:
        return False


def fetch_upscaler(folder):
    if upscaler_ready(folder):
        return
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / "download.zip"
    print(f"Downloading {UPSCALER['name']} (45 MB)...", flush=True)
    with urllib.request.urlopen(UPSCALER["url"], timeout=60) as response, open(archive, "wb") as file:
        shutil.copyfileobj(response, file)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != UPSCALER["sha256"]:
        archive.unlink()
        raise SystemExit(f"{UPSCALER['name']} did not download correctly (checksum {digest}). Try again.")
    with zipfile.ZipFile(archive) as package:
        for name in package.namelist():
            if name.endswith(".exe") or name.endswith(".dll") or name.startswith("models/"):
                package.extract(name, folder)
    archive.unlink()
    (folder / ".ready").write_text(UPSCALER["sha256"])


def chosen(dump, load, everything, again, video=True):
    done = set() if again else stems(load)
    picked = []
    skipped = {"mipmapped": 0, "small": 0, "done": 0, "video": 0}
    named = [(file, NAME.match(file.stem.lower())) for file in sorted(dump.glob("*.png"))]
    named = [(file, match) for file, match in named if match]
    shapes = {}
    for file, match in named:
        if match.group(5) in INTENSITY:
            key = (match.group(1), match.group(2), match.group(5))
            shapes[key] = shapes.get(key, 0) + 1
    for file, match in named:
        stem = file.stem.lower()
        if video and shapes.get((match.group(1), match.group(2), match.group(5)), 0) > VIDEO_FRAMES:
            skipped["video"] += 1
            continue
        if match.group(3) and not everything:
            skipped["mipmapped"] += 1
            continue
        if int(match.group(1)) < SMALLEST or int(match.group(2)) < SMALLEST:
            skipped["small"] += 1
            continue
        if stem in done:
            skipped["done"] += 1
            continue
        picked.append(file)
    return picked, skipped


def main():
    parser = argparse.ArgumentParser(description="Makes larger copies of the game's dumped textures with Real-ESRGAN, for the custom textures option.")
    parser.add_argument("--root", default=".", help="the game's folder (the one holding game/textures), the current folder by default")
    parser.add_argument("--scale", type=int, choices=(2, 4), default=4)
    parser.add_argument("--model", choices=("anime", "general", "drawn"), default="anime", help="anime suits the menus, the map and the drawn textures; general and drawn change fine details less")
    parser.add_argument("--all", action="store_true", help="also the textures with mipmaps, mostly the 3D world's")
    parser.add_argument("--again", action="store_true", help="also the textures that already have a larger copy")
    parser.add_argument("--source", help="a folder of textures named like the dumped ones to enlarge instead, without video frames (the options write every texture of the disc to one)")
    options = parser.parse_args()
    if sys.platform != "win32":
        raise SystemExit("This tool only runs on Windows for now.")
    model = MODELS.get((options.model, options.scale))
    if not model:
        raise SystemExit("The general and drawn models only exist at --scale 4.")
    root = Path(options.root).resolve()
    textures = root / "game" / "textures"
    dump = Path(options.source).resolve() if options.source else textures / "dump"
    load = textures / "load"
    if options.source and not any(dump.glob("tex1_*.png")):
        raise SystemExit(f"No textures in {dump}.")
    if not any(dump.glob("tex1_*.png")):
        raise SystemExit(f"No dumped textures in {dump}.\nTurn on 'Dump textures' in the options (Advanced), visit the menus and places you want sharper, then run this again.")
    picked, skipped = chosen(dump, load, options.all, options.again, not options.source)
    print(f"{len(picked)} textures to enlarge ({skipped['mipmapped']} with mipmaps left out, {skipped['video']} video frames, {skipped['small']} under {SMALLEST} pixels, {skipped['done']} already done).", flush=True)
    if not picked:
        return 0
    tool = root / "toolchain" / "realesrgan"
    fetch_upscaler(tool)
    target = load / "hd"
    target.mkdir(parents=True, exist_ok=True)
    work = textures / "enlarging"
    shutil.rmtree(work, ignore_errors=True)
    try:
        source = work / "in"
        result = work / "out"
        source.mkdir(parents=True)
        result.mkdir()
        empty = 0
        for file in picked:
            try:
                if not prepare(file, source):
                    empty += 1
            except (OSError, ValueError, zlib.error):
                shutil.copyfile(file, source / file.name)
        pending = sorted(source.glob("*.png"))

        def enlarge(input_path, output_path, total=0):
            command = [str(tool / "realesrgan-ncnn-vulkan.exe"), "-i", str(input_path), "-o", str(output_path), "-n", model, "-s", str(options.scale), "-t", str(TILE), "-f", "png"]
            with open(work / "upscaler.log", "w+", encoding="utf-8", errors="replace") as log:
                try:
                    process = subprocess.Popen(command, cwd=tool, stdout=subprocess.DEVNULL, stderr=log, creationflags=HIDDEN)
                except OSError as error:
                    raise SystemExit(f"The upscaler could not start: {error}\nWindows may block it (Smart App Control), or the graphics driver lacks Vulkan.")
                shown = -1
                while process.poll() is None:
                    done = sum(1 for _ in output_path.glob("*.png")) if total else shown
                    if done != shown:
                        print(f"{done}/{total}", flush=True)
                        shown = done
                    time.sleep(1)
                log.seek(0)
                return log.read()

        print(f"Enlarging {len(pending)} textures {options.scale}x with {model} on the graphics card...", flush=True)
        run = enlarge(source, result, len(pending))
        made = 0
        kept = 0
        failed = []
        again = []
        for file in pending:
            larger = result / file.name
            state = finish(file, larger)
            if state == "ok":
                shutil.move(str(larger), target / file.name)
                made += 1
            elif state == "kept":
                kept += 1
            else:
                larger.unlink(missing_ok=True)
                again.append(file)
        if again:
            print(f"{len(again)} textures came out empty, enlarging them again one at a time...", flush=True)
        for number, file in enumerate(again, 1):
            print(f"Again {number - 1}/{len(again)}: {file.name}", flush=True)
            larger = result / file.name
            enlarge(file, larger)
            state = finish(file, larger)
            if state == "ok":
                shutil.move(str(larger), target / file.name)
                made += 1
            elif state == "kept":
                kept += 1
            else:
                print(f"Still empty: {file.name}", flush=True)
                failed.append(file.name)
        if again:
            print(f"Again {len(again)}/{len(again)}", flush=True)
        if kept:
            print(f"{kept} gray textures keep their original look: the enlarged copy drifted too far from it.", flush=True)
        if failed:
            tail = "\n".join(line for line in run.splitlines()[-6:] if line.strip())
            print(f"{len(failed)} textures could not be enlarged and keep their original look:\n" + "\n".join(failed[:10]) + (f"\n{tail}" if tail else ""), flush=True)
        if empty:
            print(f"{empty} fully transparent textures left out.", flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{made} larger textures written to {target}.\nTurn on 'Custom textures' in the options (Advanced) and start the game again to see them.", flush=True)
    return 0 if made else 1


if __name__ == "__main__":
    sys.exit(main())
