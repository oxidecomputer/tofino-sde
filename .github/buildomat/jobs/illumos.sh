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
./oxide/install-dependencies.sh

banner "build"
./oxide/build.sh

banner "package"
./oxide/package.sh

pfexec mkdir -p /out
pfexec chown "$UID" /out
cp tofino_sde.p5p /out/tofino_sde.p5p
digest -a sha256 /out/tofino_sde.p5p > /out/tofino_sde.p5p.sha256.txt
