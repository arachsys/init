PREFIX =
BINDIR = ${PREFIX}/bin
DESTDIR =

CC = cc
CFLAGS = -Os -Wall -pedantic -std=gnu99
LDFLAGS =

SCRIPTS = syslogd ueventd
BINARIES = daemon pivot reap seal stop syslog uevent

all: ${SCRIPTS} ${BINARIES}

install: ${SCRIPTS} ${BINARIES}
	mkdir -p ${DESTDIR}${BINDIR}
	install -s ${BINARIES} ${DESTDIR}${BINDIR}
	install ${SCRIPTS} ${DESTDIR}${BINDIR}

clean:
	rm -f ${BINARIES}

.PHONY: install clean
