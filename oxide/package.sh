#!/bin/bash

set -e

function find_sde_version() {
	MANIFESTS=`find ${SDE} -maxdepth 1 -name "*.manifest"`
	if [ x"$MANIFESTS" == x ]; then
		echo "No manifest found - can't determine version"
		exit 1
	fi

	CNT=`/bin/ls $MANIFESTS | wc -l`
	if [ $CNT != "1" ]; then
		echo "Found multiple manifests - can't determine version"
		exit 1
	fi

	export SDE_VERSION=`basename $MANIFESTS | sed "s/bf-sde-//;s/.manifest//"`
}

function usage() {
	printf "$0 [-h] -v <version>\n"
	printf "    -h\tThis message\n"
	printf "    -t\tconstruct a simple tarball rather than a package\n"
	printf "    -v\toveride the default SDE version\n"
}

if [ x"${SDE}" == x ]; then
	export SDE=`git rev-parse --show-toplevel`
	echo Using SDE at git root: ${SDE}
else
	echo Using SDE from environment: ${SDE}
fi
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
