#!/bin/bash

# This script extracts a subset of the include files and libraries from the
# Intel Tofino SDE, and generates a Debian package containing them.  It also
# bundles the ASIC simulator, along with its libraries and support files.

export REVISION=${REVISION-1}
export LINUX_ROOT=${TOOL_ROOT}/linux

export BUILD_DIR=""

function cleanup() {
	if [ x"$BUILD_DIR" != x ]; then
		echo cleaning up $BUILD_DIR
		rm -r $BUILD_DIR
	fi
}

if [ x"$SDE" == x ]; then
	echo the SDE env variable should point to the root of the SDE tree
	exit 1
fi

if [ x"$SDE_VERSION" == x ]; then
	echo the SDE_VERSION env variable should contain the package version
	exit 1
fi
export FULLVER=$SDE_VERSION-$REVISION

export BUILD_DIR=`mktemp -d -p /tmp`
echo Building SDE in $BUILD_DIR
trap cleanup EXIT

mkdir -p $BUILD_DIR/opt/oxide/bf
for d in `cat $TOOL_ROOT/common_dir_list $LINUX_ROOT/dir_list`; do
	mkdir $BUILD_DIR/opt/oxide/bf/$d
done
for f in `cat $TOOL_ROOT/common_file_list $LINUX_ROOT/file_list`; do
	cp $SDE/install/$f $BUILD_DIR/opt/oxide/bf/$f
done

mkdir -p $BUILD_DIR/DEBIAN
cp $LINUX_ROOT/debian_sde/compat $BUILD_DIR/DEBIAN/
cp $LINUX_ROOT/debian_sde/copyright $BUILD_DIR/DEBIAN/
sed "s/_version_/$FULLVER/" $LINUX_ROOT/debian_sde/control > $BUILD_DIR/DEBIAN/control

dpkg --build $BUILD_DIR bf-sde-$FULLVER.deb

if [ $? == 0 ]; then
	rm -rf $BUILD_DIR
fi

BUILD_DIR=`mktemp -d -p /tmp`
echo Building DEV in $BUILD_DIR

mkdir -p $BUILD_DIR/opt/oxide/bf
for d in `cat $LINUX_ROOT/dev_dir_list`; do
	mkdir $BUILD_DIR/opt/oxide/bf/$d
done
for f in `cat $LINUX_ROOT/dev_file_list`; do
	cp -d $SDE/install/$f $BUILD_DIR/opt/oxide/bf/$f || exit 1
done
for f in `cat $LINUX_ROOT/dev_root_file_list`; do
	cp $SDE/$f $BUILD_DIR/opt/oxide/bf/$f || exit 1
done

mkdir -p $BUILD_DIR/DEBIAN
cp $LINUX_ROOT/debian_sde_dev/compat $BUILD_DIR/DEBIAN/
cp $LINUX_ROOT/debian_sde_dev/copyright $BUILD_DIR/DEBIAN/
sed "s/_version_/$FULLVER/" $LINUX_ROOT/debian_sde_dev/control > $BUILD_DIR/DEBIAN/control

dpkg --build $BUILD_DIR bf-sde-dev-$FULLVER.deb
