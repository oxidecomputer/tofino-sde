#!/bin/bash
#:
#: name = "linux"
#: variety = "basic"
#: target = "ubuntu-22.04"
#: output_rules = [
#:   "/out/*",
#: ]
#:
#: [[publish]]
#: series = "image"
#: name = "tofino_sde.deb"
#: from_output = "/out/tofino_sde.deb"
#
#: [[publish]]
#: series = "image"
#: name = "tofino_sde.deb.sha256.txt"
#: from_output = "/out/tofino_sde.deb.sha256.txt"

set -o errexit
set -o pipefail
set -o xtrace

banner "packages"

# This is very slow and adds a lot of stuff we don't really need. Unfortunately,
# the stock ubuntu variants of the different packages make the build unhappy.
# At some point it might be worth spending a little time getting this tool to
# install just the specific things we need.  (just abseil and boost, I think).
python3 -m pip install jsl pyinstaller
./p4studio/p4studio dependencies install

banner "build"
./oxide/build.sh
banner "package"
./oxide/package.sh

pfexec mkdir -p /out
pfexec chown "$UID" /out
cp tofino_sde.deb /out/tofino_sde.deb
digest -a sha256 /out/tofino_sde.deb > /out/tofino_sde.deb.sha256.txt
