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
#: series = "linux"
#: name = "tofino_sde.deb"
#: from_output = "/out/tofino_sde.deb"
#
#: [[publish]]
#: series = "linux"
#: name = "tofino_sde.deb.sha256.txt"
#: from_output = "/out/tofino_sde.deb.sha256.txt"

set -o errexit
set -o pipefail
set -o xtrace

function digest {
    shasum -a 256 "$1" | awk -F ' ' '{print $1}'
}

banner "packages"
./oxide/install-dependencies.sh

banner "build"
./oxide/build.sh

banner "package"
./oxide/package.sh

pfexec mkdir -p /out
pfexec chown "$UID" /out
cp tofino_sde.deb /out/tofino_sde.deb
digest /out/tofino_sde.deb > /out/tofino_sde.deb.sha256.txt
