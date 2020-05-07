PREFIX =
BINDIR = ${PREFIX}/bin
DESTDIR =

CC = gcc
CFLAGS = -Os -Wall -pedantic -std=gnu99
LDFLAGS =

SCRIPTS = syslogd ueventd
BINARIES = daemon fsync stop syslog uevent

all: ${SCRIPTS} ${BINARIES}

install: ${SCRIPTS} ${BINARIES}
	mkdir -p ${DESTDIR}${BINDIR}
	install -s ${BINARIES} ${DESTDIR}${BINDIR}
	install ${SCRIPTS} ${DESTDIR}${BINDIR}
	ln -f ${DESTDIR}${BINDIR}/fsync ${DESTDIR}${BINDIR}/fdatasync

clean:
	rm -f ${BINARIES}

.PHONY: install clean
