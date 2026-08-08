#!/bin/bash

set -ex

if [ `uname -s` == SunOS ]; then
    # Packages currently pre-built on s3. Ideally this will be part of the
    # helios repo so they can be "pkg install"ed.
    wget -P /tmp https://oxide-tofino-build.s3.us-west-2.amazonaws.com/tofino_deps.p5p
    pfexec pkg install -g /tmp/tofino_deps.p5p boost abseil || [ $? -eq 4 ] # code 4 when they're already installed
    python -m pip install jsl pyinstaller
else
    sudo apt install -y python3-pip
    python3 -m pip install jsl pyinstaller

    SDE=`git rev-parse --show-toplevel`
    export PATH=$PATH:~/.local/bin

    # This is very slow and adds a lot of stuff we don't really need. Unfortunately,
    # the stock ubuntu variants of the different packages make the build unhappy.
    # At some point it might be worth spending a little time getting this tool to
    # install just the specific things we need (just abseil, I think).
    "$SDE"/p4studio/p4studio dependencies install
fi
