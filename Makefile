CC = gcc
CFLAGS = -g -Wall -Wvla -std=c99 -fsanitize=address,undefined

all: nimd

nimd: nimd.o network.o
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $<

nimd.o network.o: network.h

clean:
	rm -f *.o nimd