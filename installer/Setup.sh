#!/bin/bash
cd "$(dirname "$0")" || exit 1

case "$(uname -m)" in
    x86_64) PYTHON_ARCH=x86_64; PYTHON_SHA256="a140c0868258075d160fa0da51ddffd423efbc9dd350695abd33e7ce3ce94352" ;;
    aarch64|arm64) PYTHON_ARCH=aarch64; PYTHON_SHA256="a8c8e1966602cc605f139d2d21c659252f9fbced1d6fb6a10fbbd59ac1366dc9" ;;
    *) PYTHON_ARCH="" ;;
esac
PYTHON_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260804/cpython-3.12.13%2B20260804-${PYTHON_ARCH}-unknown-linux-gnu-install_only.tar.gz"

say() {
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title="PokePark 2: Wonders Beyond - Setup" --text="$1" >/dev/null 2>&1
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --title "PokePark 2: Wonders Beyond - Setup" --error "$1" >/dev/null 2>&1
    fi
    echo "$1"
}

if [ -z "$PYTHON_ARCH" ]; then
    say "This setup needs a 64-bit Intel, AMD or Arm processor."
    exit 1
fi

if [ ! -d installer/sdl3/lib ]; then
    say "This setup was made for another kind of computer: installer/sdl3 is missing."
    exit 1
fi

if [ ! -x python/bin/python3 ]; then
    echo "Downloading Python..."
    rm -rf python python.tar.gz
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o python.tar.gz "$PYTHON_URL"
    else
        wget -O python.tar.gz "$PYTHON_URL"
    fi
    if [ $? -ne 0 ] || [ ! -s python.tar.gz ]; then
        rm -f python.tar.gz
        say "Python could not be downloaded. Check the internet connection and open Setup again."
        exit 1
    fi
    if [ "$(sha256sum python.tar.gz | cut -d ' ' -f 1)" != "$PYTHON_SHA256" ]; then
        rm -f python.tar.gz
        say "Python did not download correctly. Open Setup again."
        exit 1
    fi
    tar -xzf python.tar.gz && rm -f python.tar.gz
fi

exec python/bin/python3 installer/setup.py "$@"
