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
#: name = "bf_sde.p5p.sha256.txt"
#: from_output = "/out/bf_sde.p5p.sha256.txt"

set -o errexit
set -o pipefail
set -o xtrace

export PATH=$PATH:/home/build/.local/bin
banner "packages"

# Packages currently pre-built on s3.  Ideally this will be part of the
# helios repo so they can be "pkg install"ed.
wget https://oxide-tofino-build.s3.us-west-2.amazonaws.com/boost.tar.gz
(cd / ; pfexec tar xfvz /work/oxidecomputer/tofino-sde/boost.tar.gz)
wget https://oxide-tofino-build.s3.us-west-2.amazonaws.com/abseil.tar.gz
(cd / ; pfexec tar xfvz /work/oxidecomputer/tofino-sde/abseil.tar.gz)

echo pfexec pkg install cmake gcc12 bdw-gc
pwd
python -m pip install jsl pyinstaller
date

banner "build"
./oxide/build.sh
banner "package"
./oxide/package.sh

pfexec mkdir -p /out
pfexec chown "$UID" /out
pwd
ls -l
cp bf_sde.p5p /out/bf_sde.p5p
digest -a sha256 /out/bf_sde.p5p > /out/bf_sde.p5p.sha256.txt
ls -l /out
