LIB=remote_model.so

all: $(LIB)

${LIB}: remote_model.c
	gcc -shared -fPIC -ldl -o $@ $^

${SDE}/lib/${LIB}: ${LIB}
	mkdir -p ${SDE}/lib
	cp ${LIB} ${SDE}/lib/

install: ${SDE}/lib/${LIB}

clean:
	rm -f $(LIB)
