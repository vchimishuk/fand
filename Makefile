PREFIX ?= /usr/local
CFLAGS += -pedantic -Wall -Wextra -Wmissing-prototypes \
          -Wunused-function -Wshadow -Wstrict-overflow \
	  -fno-strict-aliasing -Wunused-variable \
	  -Wstrict-prototypes -Wwrite-strings -O2

all: build

build:
	${CC} ${CFLAGS} -o fand fand.c

clean:
	rm -f fand

install: build
	install -o root -g wheel -m 555 fand ${PREFIX}/sbin/fand
	install -o root -g wheel -m 444 fand.1 ${PREFIX}/man/man1/fand.1
	install -o root -g wheel -m 755 -d ${PREFIX}/share/examples/fand
	install -o root -g wheel -m 444 fand.conf ${PREFIX}/share/examples/fand/fand.conf
