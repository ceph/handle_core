CC=gcc

DESTDIR=

CFLAGS=-Wall -Wextra

all: handle_core

handle_core: handle_core.o
	$(CC) $(CFLAGS) handle_core.o -o $@

install: handle_core.o
	install -m  644 handle_core.o $(DESTDIR)/usr/lib/handle_core.o
	install -m  755 handle_core $(DESTDIR)/usr/bin/handle_core

clean:
	rm -rf *o handle_core
