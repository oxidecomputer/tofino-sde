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
#: series = "illumos"
#: name = "tofino_sde.p5p"
#: from_output = "/out/tofino_sde.p5p"
#
#: [[publish]]
#: series = "illumos"
#: name = "tofino_sde.p5p.sha256.txt"
#: from_output = "/out/tofino_sde.p5p.sha256.txt"

set -o errexit
set -o pipefail
set -o xtrace

export PATH=$PATH:/home/build/.local/bin

banner "packages"
# Packages currently pre-built on s3.  Ideally this will be part of the
# helios repo so they can be "pkg install"ed.
wget https://oxide-tofino-build.s3.us-west-2.amazonaws.com/tofino_deps.p5p
pfexec pkg install -g ./tofino_deps.p5p boost abseil

<<<<<<< HEAD
python -m pip install jsl # pyinstaller

banner "building pyinstaller"
# Temporary until https://github.com/pyinstaller/pyinstaller/issues/9280 is fixed
git clone https://github.com/Nieuwejaar/pyinstaller.git
cd pyinstaller
python -m pip install .
cd ..
=======
echo pfexec pkg install cmake gcc12 bdw-gc
python -m pip install jsl pyinstaller
>>>>>>> f7d02167 (Add support for illumos and the Sidecar platform (#3))

banner "build"
./oxide/build.sh
banner "package"
./oxide/package.sh

pfexec mkdir -p /out
pfexec chown "$UID" /out
cp tofino_sde.p5p /out/tofino_sde.p5p
digest -a sha256 /out/tofino_sde.p5p > /out/tofino_sde.p5p.sha256.txt
