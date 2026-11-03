CC     = m68k-atari-mint-gcc
CFLAGS = -Wall -Wextra -O1 -m68000 -std=gnu99

all: CARTTEST.PRG CARTSCOP.PRG CARTCONS.PRG

CARTTEST.PRG: carttest.c
	$(CC) $(CFLAGS) -s -o $@ carttest.c

CARTSCOP.PRG: carttest.c
	$(CC) $(CFLAGS) -DSCOPE -s -o $@ carttest.c

CARTCONS.PRG: cartcons.c
	$(CC) $(CFLAGS) -s -o $@ cartcons.c

clean:
	rm -f CARTTEST.PRG CARTSCOP.PRG CARTCONS.PRG
