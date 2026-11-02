CC     = m68k-atari-mint-gcc
CFLAGS = -Wall -Wextra -O1 -m68000 -std=gnu99

CARTTEST.PRG: carttest.c
	$(CC) $(CFLAGS) -s -o $@ carttest.c

clean:
	rm -f CARTTEST.PRG
