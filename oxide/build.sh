#!/bin/bash

set -e
set -x

# tweak cmake to set some additional compiler flags
function illumos_fixup_cmake {
	/usr/bin/sed -i  "
        s/^CMAKE_CXX_FLAGS:STRING=$/CMAKE_CXX_FLAGS:STRING=-D__EXTENSIONS__/;
        s/^CMAKE_C_FLAGS:STRING=$/CMAKE_C_FLAGS:STRING=-D__EXTENSIONS__ -D_POSIX_PTHREAD_SEMANTICS/ ;
        s/^CMAKE_EXE_LINKER_FLAGS:STRING=$/CMAKE_EXE_LINKER_FLAGS:STRING=-lnsl -lsocket/ " \
        ${SDE}/build/CMakeCache.txt

}

function configure_build {
	# We only want to build the sidecar code on helios
if [ $ILLUMOS -eq 0 ]; then
	BSP=""
else
	BSP="bsp"
fi

	cd $SDE/p4studio
	./p4studio configure asic ^tofino tofino2 bfrt ^thrift-driver ^grpc \
                ^bfrt-generic-flags ^thrift-diags ^bf-diags ^thrift-switch  \
                ^switch ^sai ^tcmalloc ^bf-python ^kernel-modules ${BSP}
}

function build {
	cd $SDE/build
	${MAKE} -j ${JOBS} install
	if [ $ILLUMOS -eq 0 ]; then
		(cd ${SDE}/oxide/remote_model ; SDE=${SDE_INSTALL} make install)
	fi
}


function usage() {
	printf "$0 [-h] [-v <version>] [-j <jobs>] [-s <sde_tarball>]\n"
	printf "    -h\tThis message\n"
	printf "    -j\tTell (g)make how many jobs to spawn\n"
}

function install_packages() {
	pfexec pkg install protobuf
}

if [ x"${SDE}" == x ]; then
	export SDE=`git rev-parse --show-toplevel`
	echo Using SDE at git root: ${SDE}
else
	echo Using SDE from environment: ${SDE}
fi

export JOBS=8
while getopts hs:j:v: opt; do
	if [ $opt == "h" ]; then
		usage $0
		exit 0
	elif [ $opt == "j" ]; then
		JOBS=$OPTARG
	else
		usage $0
		exit 1
	fi
done

if [ `uname -s` == SunOS ]; then
	export ILLUMOS=1
	export MAKE=gmake
	export PATH=${PATH}:/usr/lib/python3.11/bin
else
	export ILLUMOS=0
	export MAKE=make
	alias gmake=make
fi

configure_build

[ $ILLUMOS -eq 1 ] && illumos_fixup_cmake

build
