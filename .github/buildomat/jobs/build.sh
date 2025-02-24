#!/bin/bash
#:
#: name = "build"
#: variety = "basic"
#: target = "helios"
#: output_rules = [
#:   "/out/*",
#: ]
#:
#: [[publish]]
#: series = "image"
#: name = "bf_sde.p5p"
#: from_output = "/out/bf_sde.p5p"
#
#: [[publish]]
#: series = "image"
#: name = "bf_sde.tgz"
#: from_output = "/out/bf_sde.tgz"
#:
#: [[publish]]
#: series = "image"
#: name = "bf_sde.p5p.sha256.txt"
#: from_output = "/out/bf_sde.p5p.sha256.txt"
#:
#: [[publish]]
#: series = "image"
#: name = "bf_sde.tgz.sha256.txt"
#: from_output = "/out/bf_sde.tgz.sha256.txt"
#:

set -o errexit
set -o pipefail
set -o xtrace

export PATH=$PATH:/home/build/.local/bin
banner "packages"
pfexec pkg install library/libusb
python -m pip install wheel
python -m pip install click click_logging cmakeast pyyaml jsonschema

banner "build"
./oxide/build.sh
banner "package"
./oxide/package.sh
./oxide/package.sh -t

pfexec mkdir -p /out
pfexec chown "$UID" /out
pwd
ls -l
cp bf_sde.p5p /out/bf_sde.p5p
digest -a sha256 /out/bf_sde.p5p > /out/bf_sde.p5p.sha256.txt
cp bf_sde.tgz /out/bf_sde.tgz
digest -a sha256 /out/bf_sde.tgz > /out/bf_sde.tgz.sha256.txt
ls -l /out
