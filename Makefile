BINDIR := $(PREFIX)/bin
CFLAGS := -Os -Wall -Wfatal-errors

SCRIPTS := syslogd ueventd
BINARIES := daemon pivot reap seal stop syslog uevent

%:: %.c Makefile
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

all: $(SCRIPTS) $(BINARIES)

install: $(SCRIPTS) $(BINARIES)
	mkdir -p $(DESTDIR)$(BINDIR)
	install -s $(BINARIES) $(DESTDIR)$(BINDIR)
	install $(SCRIPTS) $(DESTDIR)$(BINDIR)

clean:
	rm -f $(BINARIES)

.PHONY: all install clean
