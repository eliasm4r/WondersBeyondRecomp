#!/bin/bash
cd "$(dirname "$0")" || exit 1

PYTHON_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260804/cpython-3.12.13%2B20260804-aarch64-apple-darwin-install_only.tar.gz"
PYTHON_SHA256="30f8ac8fc03967ff58d4a32e4882b1f3732e703db404bc1d9c52e22faa7a5dd3"

say() {
    osascript -e "display dialog \"$1\" with title \"PokePark 2: Wonders Beyond - Setup\" buttons {\"OK\"} default button \"OK\" with icon caution" >/dev/null 2>&1
    echo "$1"
}

if [ "$(uname -m)" != "arm64" ]; then
    say "This setup needs a Mac with Apple Silicon (M1 or later)."
    exit 1
fi

if ! xcode-select -p >/dev/null 2>&1 || ! /usr/bin/xcrun --find clang >/dev/null 2>&1; then
    xcode-select --install >/dev/null 2>&1
    say "The game is built with Apple's Command Line Tools. Click Install in the window that just opened, wait until it is done, then open Setup again."
    exit 1
fi

if [ ! -x python/bin/python3 ]; then
    echo "Downloading Python..."
    rm -rf python python.tar.gz
    if ! curl -fL --retry 3 -o python.tar.gz "$PYTHON_URL"; then
        say "Python could not be downloaded. Check the internet connection and open Setup again."
        exit 1
    fi
    if [ "$(shasum -a 256 python.tar.gz | cut -d ' ' -f 1)" != "$PYTHON_SHA256" ]; then
        rm -f python.tar.gz
        say "Python did not download correctly. Open Setup again."
        exit 1
    fi
    tar -xzf python.tar.gz && rm -f python.tar.gz
fi

exec python/bin/python3 installer/setup.py
