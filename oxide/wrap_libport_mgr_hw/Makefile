CC=/opt/gcc-12/bin/gcc
AR=/usr/bin/ar
LD=/usr/bin/ld
RM=/usr/bin/rm
CP=/usr/bin/cp
RMDIR=/usr/bin/rmdir

SDE := $(shell  git rev-parse --show-toplevel)
ORIG_LIB=libport_mgr_hw.a.orig
WRAPPED_LIBS=libport_mgr_hw.a libport_mgr_hw.so
LIB_DIR=${SDE}/pkgsrc/bf-drivers/src/port_mgr/port_mgr_lib

all: ${WRAPPED_LIBS}

${ORIG_LIB}:
	${CP} ${LIB_DIR}/libport_mgr_hw.a ${ORIG_LIB}

object_files:
	mkdir object_files

object_files/.done: object_files ${ORIG_LIB}
	${AR} -t ${ORIG_LIB} > object_files/object_names
	(cd object_files; ${AR} -x ../${ORIG_LIB} `cat object_names`; touch .done)

object_files/shim.o: shim.c
	${CC} -c -fpic -o object_files/shim.o shim.c

libport_mgr_hw.o:	object_files/.done object_files/shim.o
	${LD} --relocatable object_files/*.o -o $@

libport_mgr_hw.a: libport_mgr_hw.o
	rm -f $@
	${AR} -qv $@ $^

libport_mgr_hw.so: libport_mgr_hw.o
	${CC} -G -o $@ -h $@ -fpic $^

install: ${WRAPPED_LIBS}
	${CP} ${WRAPPED_LIBS} ${LIB_DIR}

clean:
	${RM} -f object_files/.done \
		object_files/*.o \
		object_files/object_names \
		libport_mgr_hw.o \
		${WRAPPED_LIBS} \
		${ORIG_LIB}
	${RMDIR} object_files
