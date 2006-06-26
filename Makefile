PREFIX =
BINDIR = ${PREFIX}/bin
DESTDIR =

CC = gcc
CFLAGS = -Os -Wall -pedantic -std=gnu99
LDFLAGS =

SCRIPTS = halt poweroff reboot single
BINARIES = init run-init

all: ${SCRIPTS} ${BINARIES}

install: ${SCRIPTS} ${BINARIES}
	mkdir -p ${DESTDIR}${BINDIR}
	install -s ${BINARIES} ${DESTDIR}${BINDIR}
	install ${SCRIPTS} ${DESTDIR}${BINDIR}

clean:
	rm -f ${BINARIES}

.PHONY: install clean
