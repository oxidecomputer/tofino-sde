#!/bin/bash

# This script extracts a subset of the include files and libraries from the
# Intel Tofino SDE, and generates an Illumos package containing them.  The
# files should be limited to those needed to build and run the Dendrite
# controller stack.

BASE_FMRI=pkg://oxide/internal/bf_sde

if [ $BUILD_TARBALL == 0 ];
then
	ARCHIVE=bf_sde.p5p
else
	ARCHIVE=bf_sde.tgz
fi

if [ -e $ARCHIVE ]; then
	echo $ARCHIVE already exists
	exit 1
fi

if [ x"$SDE" == x ]; then
	echo the SDE env variable should point to the root of the SDE install directory
	exit 1
fi

if [ x"$SDE_VERSION" == x ]; then
	echo the SDE_VERSION env variable should contain the package version
	exit 1
fi

function cleanup() {
	if [ x"$BUILD_DIR" != x ]; then
		echo cleaning up $BUILD_DIR
		rm -r $BUILD_DIR
	fi
	if [ x"$REPO_DIR" != x ]; then
		echo cleaning up $REPO_DIR
		rm -r $REPO_DIR
	fi
	if [ x"$MANIFEST" != x ]; then
		echo cleaning up $MANIFEST
		rm $MANIFEST
	fi
}

function build_p5p() {
	MANIFEST=`/usr/bin/mktemp`
	FMRI=${BASE_FMRI}@${SDE_VERSION}
	/usr/bin/sed "s#FMRI#$FMRI#" $TOOL_ROOT/helios/tofino_sde.mf.base > $MANIFEST
	# hack to avoid conflicting with /opt created by on-nightly packages
	echo "dir  path=opt owner=root group=sys mode=0755" >> $MANIFEST
	pkgsend generate $BUILD_DIR | egrep -v "path=opt$" | pkgfmt >> $MANIFEST
	if [ $? != 0 ]; then
		exit 1
	fi

	REPO_DIR=/var${BUILD_DIR}
	echo Building the repo in $REPO_DIR
	pkgrepo create $REPO_DIR
	if [ $? != 0 ]; then
		exit 1
	fi

	pkgsend publish -d $BUILD_DIR -s $REPO_DIR $MANIFEST
	if [ $? != 0 ]; then
		exit 1
	fi

	echo Building the package archive
	pkgrecv -a -d $ARCHIVE -s $REPO_DIR $FMRI
	if [ $? != 0 ]; then
		exit 1
	fi
}

function build_tarball() {
	echo building tarball
	tar cfz ${ARCHIVE} -C $BUILD_DIR opt/oxide/bf_sde
}

BUILD_DIR=`/usr/bin/mktemp -d`
echo Building the proto area in $BUILD_DIR
trap cleanup EXIT

/usr/bin/mkdir -p $BUILD_DIR/opt/oxide/bf_sde
for d in `cat $TOOL_ROOT/common_dir_list` `cat $TOOL_ROOT/helios/dir_list`; do
	/usr/bin/mkdir -p $BUILD_DIR/opt/oxide/bf_sde/$d
done
for f in `cat $TOOL_ROOT/common_file_list` `cat $TOOL_ROOT/helios/file_list`; do
	/usr/bin/cp $SDE_INSTALL/$f $BUILD_DIR/opt/oxide/bf_sde/$f
done
git rev-list HEAD -1 > $BUILD_DIR/opt/oxide/bf_sde/version

if [ $BUILD_TARBALL == 1 ]; then
	build_tarball
else
	build_p5p
fi
