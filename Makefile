CC=gcc

liblocation=$(CURDIR)/debian/handle-core/usr/lib
binlocation=$(CURDIR)/debian/handle-core/usr/bin

CFLAGS=-Wall -Wextra

all: handle_core

handle_core: handle_core.o
	$(CC) $(CFLAGS) handle_core.o -o $@

install: handle_core.o
	install -m  644 handle_core.o $(liblocation)
	install -m  755 handle_core $(binlocation)

clean:
	rm -rf *o handle_core
