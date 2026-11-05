# CART_NET.STX: STinG port driver for TheHulk (m68k-atari-mint-gcc)
# -mshort: STinG passes 16-bit ints. No C library: only libgcc.
CC = m68k-atari-mint-gcc
CFLAGS = -m68000 -mshort -O2 -Wall -fomit-frame-pointer -fno-builtin -Iinclude
OBJS = init.o cart_net.o cartnet.o

CART_NET.STX: $(OBJS)
	$(CC) -mshort -nostdlib -o $@ $(OBJS) -lgcc -s

%.o: %.c include/port.h include/transprt.h cartnet.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f CART_NET.STX $(OBJS)
