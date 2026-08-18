.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c99 -pedantic -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L \
          -Dtypeof=__typeof__ -DHML_VERSION='"$(VERSION)"' \
          -isystem vendor -pthread
LDLIBS  = -lssl -lcrypto
BINDIR  = $(HOME)/.local/bin
OBJ     = hml.o sync.o send.o imap.o state.o maildir.o config.o

all: hml

hml: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): hml.h

.c.o:
	$(CC) $(CFLAGS) -c $<

install: hml
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hml" $(BINDIR)/hml

uninstall:
	rm -f $(BINDIR)/hml

clean:
	rm -f hml $(OBJ)

.PHONY: all install uninstall clean
