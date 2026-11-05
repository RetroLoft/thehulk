/*
 * Cartridge transport for CART_NET.STX: commands to TheHulk over the cartridge port, as
 * GEMDRIVE sends them (esp32/main/gemdrv_layout.h, GEMDRVEMUL_NET_*). Compiled with -mshort.
 */
#ifndef CARTNET_H
#define CARTNET_H

#define NET_INFO    0x0620
#define NET_CTRL    0x0621
#define NET_TX      0x0622
#define NET_RX      0x0623

#define NET_INFO_SIZE   128
#define NET_HDR         8

#define ST_OK       0L
#define ST_TIMEOUT  (-1L)
#define ST_LOCKED   (-2L)       /* GEMDRIVE (or we ourselves) is using the cartridge: skip */

/* TheHulk in the cartridge port: 0, else -1. Supervisor mode. */
int cart_find(void);

/* One command. Supervisor mode. Sends len bytes of buf (write), or fills buf with the
   answer (NET_INFO: NET_INFO_SIZE bytes; NET_RX: header and frame, at most len bytes).
   Returns the status of TheHulk (0 = ok), ST_TIMEOUT or ST_LOCKED. */
long cart_net(int cmd, void *buf, int len, int write);

/* Frames waiting for the Atari, read without a command */
int cart_pending(void);

#endif
