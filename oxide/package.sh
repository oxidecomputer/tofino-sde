#!/bin/bash

set -e

function find_sde_version() {
	IFACE_VERSION=`cat ${SDE}/install/share/VERSION`
	export SDE_VERSION=$IFACE_VERSION-`/usr/bin/date +'%Y%m%d'`
}

function usage() {
	printf "$0 [-h] -v <version>\n"
	printf "    -h\tThis message\n"
	printf "    -t\tconstruct a simple tarball rather than a package\n"
	printf "    -v\toveride the default SDE version\n"
}

export SDE=`git rev-parse --show-toplevel`
echo Using SDE at git root: ${SDE}

export SDE_INSTALL=${SDE}/install
export TOOL_ROOT=${SDE}/oxide/package
find_sde_version

BUILD_TARBALL=0

while getopts htv: opt; do
	if [ $opt == "h" ]; then
		usage $0
		exit 0
	elif [ $opt == "v" ]; then
		SDE_VERSION=$OPTARG
	elif [ $opt == "t" ]; then
		BUILD_TARBALL=1
	else
		usage $0
		exit 1
	fi
done

if [ `uname -s` == SunOS ]; then
	source ${SDE}/oxide/package/helios/package.sh
else
	source ${SDE}/oxide/package/linux/package.sh
fi
