LIBDIR=${SDE}/install/lib
LIB=remote_model.so

all: ${LIB}

${LIB}: remote_model.c
	gcc -shared -fPIC -ldl -o $@ $^

install: ${LIB}
	mkdir -p ${LIBDIR}
	cp ${LIB} ${LIBDIR}

clean:
	rm -f $(LIB)
