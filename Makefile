CC=gcc
CFLAGS=-std=gnu99 -Wall -Wextra -g

all: wish

wish: wish.c
	$(CC) $(CFLAGS) -o wish wish.c

clean:
	rm -f wish
