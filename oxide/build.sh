#!/bin/bash

set -e
set -x

# tweak cmake to set some additional compiler flags
function illumos_fixup_cmake {
	/usr/bin/sed -i  "
        s/^CMAKE_CXX_FLAGS:STRING=$/CMAKE_CXX_FLAGS:STRING=-D__EXTENSIONS__/;
        s/^CMAKE_C_FLAGS:STRING=$/CMAKE_C_FLAGS:STRING=-D__EXTENSIONS__ -D_POSIX_PTHREAD_SEMANTICS/ ;
        s/^CMAKE_EXE_LINKER_FLAGS:STRING=$/CMAKE_EXE_LINKER_FLAGS:STRING=-lnsl -lsocket/ "  \
        ${SDE}/build/CMakeCache.txt

}

function configure_build {
	# We only want to build the sidecar code on helios
if [ $ILLUMOS -eq 0 ]; then
	BSP=OFF
	LINKER_FLAGS=""
	ABSL_DIR="-Dabsl_DIR=/usr/lib/x86_64-linux-gnu/cmake/absl"
	CXX_FLAGS="-I${SDE}/oxide/rapidjson/include"
	C_FLAGS=""
else
	BSP=ON
	LINKER_FLAGS="-lnsl -lsocket"
	CXX_FLAGS="-D__EXTENSIONS__ -I${SDE}/oxide/rapidjson/include"
	C_FLAGS="-D__EXTENSIONS__ -D_POSIX_PTHREAD_SEMANTICS"
fi

	cd ${SDE}/build
	cmake $SDE \
		-DASIC=ON  \
		-DTOFINO=OFF \
		-DTOFINO2=ON \
		-DBFRT=ON \
		-DBFRT-GENERIC-FLAGS=OFF \
		-DTHRIFT-DRIVER=OFF \
		-DTHRIFT-DIAGS=OFF \
		-DTHRIFT-SWITCH=OFF \
		-DGRPC=OFF \
		-DBF-DIAGS=OFF \
		-DSWITCH=OFF \
		-DSAI=OFF \
		-DTCMALLOC=OFF \
		-DBF-PYTHON=OFF \
		-DKERNEL-MODULES=OFF \
		-DBSP=$BSP \
		-DRAPIDJSON_DIR=$RAPIDJSON_DIR/include \
		-DP4C_USE_PREINSTALLED_ABSEIL=ON  \
		${ABSL_DIR} \
		-DP4C_USE_PREINSTALLED_PROTBUF=OFF  \
		-DUSE_PREINSTALLED_PROTBUF=OFF  \
		-DBoost_INCLUDE_DIRS=/opt/ooce/boost/include  \
		-DBoost_USE_STATIC_RUNTIME=ON  \
		-DCMAKE_BUILD_TYPE='Release' \
		-DCMAKE_LINKER='lld' \
		-DCMAKE_INSTALL_PREFIX=$SDE/install \
		-DCMAKE_CXX_FLAGS=$CXX_FLAGS \
		-DCMAKE_C_FLAGS=$C_FLAGS \
		-DCMAKE_EXE_LINKER_FLAGS=${LINKER_FLAGS}
}

function build {
	cd $SDE/build
	${MAKE} -j ${JOBS} install
	if [ $ILLUMOS -eq 0 ]; then
		(cd ${SDE}/oxide/remote_model ; SDE=${SDE_INSTALL} make install)
	fi
}


function usage() {
	printf "$0 [-h] [-v <version>] [-j <jobs>]\n"
	printf "    -h \tThis message\n"
	printf "    -j \tTell (g)make how many jobs to spawn\n"
}

function install_packages() {
	pfexec pkg install bdw-gc pyinstaller # protobuf
	pip3 install jsl
}

export SDE=`git rev-parse --show-toplevel`
echo Building SDE at git root: ${SDE}
mkdir ${SDE}/build || echo ${SDE}/build already exists

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
	#export PATH=${PATH}:/usr/lib/python3.11/bin:/usr/gnu/bin/:~/.local/bin
	echo Wrapping wrap_libport_mgr_hw
	(cd wrap_libport_mgr_hw ; gmake install)
else
	export ILLUMOS=0
	export MAKE=make
	alias gmake=make
fi

export PATH=${SDE}/oxide/python_venv/bin:${PATH}/usr/gnu/bin/:~/.local/bin
RAPIDJSON_DIR=${SDE}/oxide/rapidjson
if [ ! -d $RAPIDJSON_DIR ]; then
	(cd ${SDE}/oxide ; git clone https://github.com/Tencent/rapidjson.git)
fi

# helios: pfexec pkg install boost
# linux: sudo apt install cmake python3-distutils
# both: pip3 install jsl
configure_build

# [ $ILLUMOS -eq 1 ] && illumos_fixup_cmake

build
