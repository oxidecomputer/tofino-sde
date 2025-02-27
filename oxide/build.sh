#!/bin/bash

set -e
set -x

if [ `uname -s` == SunOS ]; then
    export ILLUMOS=1
else
    export ILLUMOS=0
fi

function get_firmware() {
    FW_DIR=${SDE}/install/share/tofino_sds_fw/credo
    mkdir -p $FW_DIR
    wget -P /tmp https://oxide-tofino-build.s3.us-west-2.amazonaws.com/serdes_fw.tar.gz
    (cd $FW_DIR ; tar xvfz /tmp/serdes_fw.tar.gz)
}

function patch_abseil() {
    (
        cd ${SDE}/install/include/absl/container/internal/
	cat << EOF | patch -p1 layout.h
--- layout.h.orig	2025-02-20 21:52:10.936645134 +0000
+++ layout.h	2025-02-20 21:55:01.170413186 +0000
@@ -301,7 +301,7 @@
 #endif
   if (status == 0 && demangled != nullptr) {  // Demangling succeeded.
     absl::StrAppend(&out, "<", demangled, ">");
-    free(demangled);
+    std::free(demangled);
   } else {
 #if defined(__GXX_RTTI) || defined(_CPPRTTI)
     absl::StrAppend(&out, "<", typeid(T).name(), ">");
EOF
    )
}

function prework() {
    if [ $ILLUMOS -eq 1 ]; then
        echo Wrapping wrap_libport_mgr_hw
        (cd ${SDE}/oxide/wrap_libport_mgr_hw ; gmake clean; gmake install)
    else
        patch_abseil
    fi

    RAPIDJSON_DIR=${SDE}/oxide/rapidjson
    if [ ! -d $RAPIDJSON_DIR ]; then
        (cd ${SDE}/oxide ; git clone https://github.com/Tencent/rapidjson.git)
    fi
}

function configure_build {
    if [ $ILLUMOS -eq 0 ]; then
        # We only want to build the sidecar code on helios
        BSP=OFF
        LINKER_FLAGS=""
        BOOST_DIR=${SDE}/install/include/boost/include
        BOOST_STATIC=OFF
        ABSL_DIR="/usr/lib/x86_64-linux-gnu/cmake/absl"
        CXX_FLAGS="-I${SDE}/oxide/rapidjson/include"
        C_FLAGS=""
    else
        BSP=ON
        LINKER_FLAGS="-lnsl -lsocket"
        BOOST_DIR=/opt/ooce/boost/include
        BOOST_STATIC=ON
        CXX_FLAGS="-D__EXTENSIONS__ -I${SDE}/oxide/rapidjson/include"
        C_FLAGS="-D__EXTENSIONS__ -D_POSIX_PTHREAD_SEMANTICS"
	# To pick up realpath and the pip installed pyinstaller.
	# XXX: this should be part of the CI controller, not here
	PATH=${PATH}:~/.local/bin:/usr/gnu/bin
    fi

    cd ${SDE}/build
    CMAKE_PREFIX_PATH=/usr/lib/x86_64-linux-gnu/cmake/absl \
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
        -DP4C_USE_PREINSTALLED_ABSEIL=ON \
        -DP4C_USE_PREINSTALLED_PROTOBUF=OFF \
        -DBoost_INCLUDE_DIRS=${BOOST_DIR} \
	-DBoost_USE_STATIC_RUNTIME=$BOOST_STATIC \
        -DCMAKE_BUILD_TYPE='Release' \
        -DCMAKE_LINKER='lld' \
        -DCMAKE_INSTALL_PREFIX=$SDE/install \
        -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$LINKER_FLAGS" \
        -DCMAKE_C_FLAGS="$C_FLAGS"
}

function build {
    cd $SDE/build
    gmake -j ${JOBS} install
    if [ $ILLUMOS -eq 1 ]; then
	get_firmware
    else
        (cd ${SDE}/oxide/remote_model ; make install)
    fi
}


function usage() {
    printf "$0 [-h] [-v <version>] [-j <jobs>]\n"
    printf "    -h \tThis message\n"
    printf "    -j \tTell (g)make how many jobs to spawn\n"
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

if [ $ILLUMOS -eq 1 ]; then
    alias make=gmake
fi

prework
configure_build
build
