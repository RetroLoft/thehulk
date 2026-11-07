/*
 * Cartridge transport for CART_NET.STX, see cartnet.h. The cartridge port can only be
 * read: a command is a read of $FBABCD, then each word as two reads at $FB0000 + 2 * byte,
 * high byte first. TheHulk answers in ROM3 and echoes the token at $FB0000.
 *
 * STinG calls the driver from its 200 Hz interrupt. A command from GEMDRIVE or THEHULK.PRG
 * may be in progress underneath: they then have the busy flag set (CODE_BUFFER_BUSY in
 * gemdrive.s) and we do nothing this time, as ACSI_NET.STX does with flock.
 * Outside its own commands the driver must not read ROM3 at all: the gateware would take
 * such a read as a byte of the command being sent (seen on the Falcon: a 0 byte slipped
 * into write blocks and drive settings). So the busy flag's address is read once, when the
 * driver loads, and the "frames waiting" word comes from its copy in ROM4.
 */
#include "cartnet.h"

#define ROM3            0xFB0000L
#define ROM4            0xFA0000L
#define HEADER          (ROM3 + 0xABCDL)    /* CMD_MAGIC_NUMBER */
#define TOKEN           0x0000L
#define SEED            0x0004L
#define SHARED_VARS     0x4198L             /* GEMDRVEMUL_SHARED_VARIABLES */
#define VAR_CODE_BUFFER (16 + 20)           /* SHARED_VARIABLE_CODE_BUFFER */
#define CODE_BUFFER_BUSY 94                 /* gemdrive.s */
#define NET_PENDING_ROM4 0xfff0L            /* GEMDRVEMUL_NET_PENDING_ROM4 */
#define NET_STATUS      0x6004L
#define NET_INFO_AREA   0x6010L
#define NET_RX_AREA     0x6100L
#define CART_MAGIC      0xABCDEF42L
#define TOKEN_POLLS     500000L             /* > 1 s on a Falcon: TheHulk answers in ms */

static volatile int in_use;
static unsigned long code_buffer;          /* GEMDRIVE's, with the busy flag */

static unsigned long rom3_long(unsigned long offset)
{
    return *(volatile unsigned long *)(ROM3 + offset);
}

static void send_word(unsigned int w)
{
    (void)*(volatile unsigned char *)(ROM3 + ((unsigned long)(w >> 8 & 0xff) << 1));
    (void)*(volatile unsigned char *)(ROM3 + ((unsigned long)(w & 0xff) << 1));
}

static int gemdrive_busy(void)
{
    return code_buffer && *(volatile unsigned char *)(code_buffer + CODE_BUFFER_BUSY);
}

/* Process context (loading, the installer): no command can be half sent here */
int cart_find(void)
{
    if (*(volatile unsigned long *)ROM4 != CART_MAGIC)
        return -1;
    code_buffer = rom3_long(SHARED_VARS + VAR_CODE_BUFFER * 4L);
    return 0;
}

int cart_pending(void)
{
    return *(volatile unsigned short *)(ROM4 + NET_PENDING_ROM4);
}

long cart_net(int cmd, void *buf, int len, int write)
{
    unsigned char *b = buf;
    unsigned long seed, i, n;
    unsigned short sr;
    long r = ST_TIMEOUT;

    /* in_use: the STinG interrupt may come while a command from process context runs */
    __asm__ volatile ("move.w %%sr,%0\n\tori.w #0x700,%%sr" : "=d"(sr) : : "cc", "memory");
    if (in_use || gemdrive_busy()) {
        __asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "cc", "memory");
        return ST_LOCKED;
    }
    in_use = 1;
    __asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "cc", "memory");

    n = write ? (unsigned long)(len + 1) & ~1UL : 0;
    seed = rom3_long(SEED);
    (void)*(volatile unsigned char *)HEADER;
    send_word((unsigned int)cmd);
    send_word((unsigned int)(n + 4));
    send_word((unsigned int)(seed & 0xffff));
    send_word((unsigned int)(seed >> 16));
    for (i = 0; i < n; i += 2)
        send_word((unsigned int)b[i] << 8 | (i + 1 < (unsigned long)len ? b[i + 1] : 0));

    for (i = 0; i < TOKEN_POLLS; i++)
        if (rom3_long(TOKEN) == seed)
            break;
    if (i < TOKEN_POLLS) {
        r = (long)rom3_long(NET_STATUS);
        if (!write) {
            const volatile unsigned short *src = (const volatile unsigned short *)
                (ROM3 + (cmd == NET_INFO ? NET_INFO_AREA : NET_RX_AREA));
            unsigned short *dst = (unsigned short *)b;
            unsigned long words = cmd == NET_INFO ? NET_INFO_SIZE / 2 : (NET_HDR + 1) / 2;
            if (cmd == NET_RX) {
                unsigned long frame = src[0];       /* length of the frame */
                words = (NET_HDR + frame + 1) / 2;
            }
            if (words > (unsigned long)len / 2)
                words = (unsigned long)len / 2;
            for (i = 0; i < words; i++)
                dst[i] = src[i];
        }
    }
    in_use = 0;
    return r;
}
