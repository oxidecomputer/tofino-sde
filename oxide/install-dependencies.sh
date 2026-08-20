#!/bin/bash

set -ex

# https://github.com/pyinstaller/pyinstaller/issues/9280
PYINSTALLER_VERSION="pyinstaller>=6.17"

if [ "$(uname -s)" = SunOS ]; then
    # Packages currently pre-built on s3. Ideally this will be part of the
    # helios repo so they can be "pkg install"ed.
    wget -O /tmp/tofino_deps.p5p https://oxide-tofino-build.s3.us-west-2.amazonaws.com/tofino_deps.p5p

    rc=0; pfexec pkg install -g /tmp/tofino_deps.p5p boost abseil gcc12 || rc=$?
    # pkg exits 4 when the packages are already installed.
    [ "$rc" -eq 0 ] || [ "$rc" -eq 4 ] || exit "$rc"

    python -m pip install jsl "$PYINSTALLER_VERSION"
else
    sudo apt install -y python3-pip gcc-11 g++-11
    python3 -m pip install jsl "$PYINSTALLER_VERSION"

    SDE=$(git rev-parse --show-toplevel)
    export PATH=$PATH:~/.local/bin

    # This is very slow and adds a lot of stuff we don't really need. Unfortunately,
    # the stock ubuntu variants of the different packages make the build unhappy.
    # At some point it might be worth spending a little time getting this tool to
    # install just the specific things we need (just abseil, I think).
    "$SDE"/p4studio/p4studio dependencies install
fi
