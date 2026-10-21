CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: livelog livelog-decrypt

livelog: src/livelog.c src/rc4.h
	$(CC) $(CFLAGS) -o $@ src/livelog.c -lutil

livelog-decrypt: src/livelog-decrypt.c src/rc4.h
	$(CC) $(CFLAGS) -o $@ src/livelog-decrypt.c

test: all
	sh tests/run.sh

clean:
	rm -f livelog livelog-decrypt

.PHONY: all test clean
