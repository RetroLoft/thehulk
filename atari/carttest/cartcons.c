/* Reads the known patterns from the cartridge, from the DIAG_CONST gateware or the
 * HULK_DIAG_PATTERN firmware, with the cartridge code out of the way: start it from a hard
 * disk. Results go to the screen and to CARTCONS.TXT beside the program.
 */
#include <osbind.h>

#define WORDS       1792L      // from +$200 up to the end of a range of $1000 bytes
#define PASSES      200
#define RANGES      9       /* the last one with A15 high */
#define STYLES      3
#define MAX_SHOWN   4       // wrong words shown per test
#define ROM4        0xFA0000L
/* With GEMDRIVE on the cartridge: only FFFF and 0000, in parts it does not use, see HULK_DIAG_HIGH */
#define ROM3_FFFF   0xFB8000L
#define ROM3_0000   0xFB6000L
#define RANGES_HIGH 2
#define CART_MAGIC  0xABCDEF42L
#define ROM3        0xFB0000L

static const char *const range_name[RANGES] = {
    "FFFF     ", "0000     ", "AAAA     ", "5555     ", "0000/FFFF", "00FF     ", "FF00     ", "0001     ", "FFFF A15 "
};
static const char *const style_name[STYLES] = { "long", "word", "byte" };

static unsigned short got[WORDS];
static short report = -1;
static int shown;
static int gemdrive;
static unsigned long whole_zero, partial;

static void out(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    (void)Cconws(s);
    if (report >= 0)
        Fwrite(report, n, s);
}

static void out_dec(unsigned long v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = '0' + v % 10;
        v /= 10;
    } while (v);
    out(b + i);
}

static void out_hex(unsigned long v, int digits)
{
    char b[9];
    b[digits] = 0;
    while (digits--) {
        b[digits] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    }
    out(b);
}

static void read_block(int style, long addr)
{
    const volatile void *src = (const volatile void *)addr;
    void *dst = got;
    if (style == 0) {
        long n = WORDS / 2 - 1;
        __asm__ volatile ("1: move.l (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
    } else if (style == 1) {
        long n = WORDS - 1;
        __asm__ volatile ("1: move.w (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
    } else {
        long n = WORDS * 2 - 1;
        __asm__ volatile ("1: move.b (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
    }
}

static unsigned short expected(int range, long i)
{
    static const unsigned short fixed[RANGES] = {
        0xFFFF, 0x0000, 0xAAAA, 0x5555, 0, 0x00FF, 0xFF00, 0x0001, 0xFFFF
    };
    return range == 4 ? (i & 1 ? 0xFFFF : 0x0000) : fixed[range];
}

static void test_range(int range, int style)
{
    /* Inside the range, away from its start: the output needs a moment after the switch */
    long addr = (gemdrive ? (range ? ROM3_0000 : ROM3_FFFF)
                          : range == 8 ? ROM3_FFFF : ROM3 + ((long)range << 12)) + 0x200;
    unsigned long total = 0, wrong = 0, lost = 0, gained = 0;
    shown = 0;
    whole_zero = partial = 0;
    read_block(style, addr);            /* settles the value, not counted */
    for (int pass = 0; pass < PASSES; pass++) {
        read_block(style, addr);
        total += WORDS;
        for (long i = 0; i < WORDS; i++) {
            unsigned short e = expected(range, i), g = got[i], d = e ^ g;
            if (!d)
                continue;
            wrong++;
            if (g == 0)
                whole_zero++;
            else
                partial++;
            for (int b = 0; b < 16; b++)
                if (d >> b & 1) {
                    if (e >> b & 1)
                        lost++;
                    else
                        gained++;
                }
            if (shown < MAX_SHOWN) {
                shown++;
                out(" at ");
                out_hex(addr + i * 2, 6);
                out(" want ");
                out_hex(e, 4);
                out(" got ");
                out_hex(g, 4);
                out("\r\n");
            }
        }
    }
    out(range_name[range]);
    out(" ");
    out(style_name[style]);
    out(" wrong ");
    out_dec(wrong);
    out(" of ");
    out_dec(total);
    out(", lost ");
    out_dec(lost);
    out(", gained ");
    out_dec(gained);
    out(", 0000 ");
    out_dec(whole_zero);
    out(", other ");
    out_dec(partial);
    out("\r\n");
}

int main(void)
{
    long h = Fcreate("CARTCONS.TXT", 0);
    if (h >= 0)
        report = h;
    long stack = Super(0L);
    unsigned long sysbase = *(volatile unsigned long *)0x4F2L;
    unsigned short tos = *(volatile unsigned short *)(sysbase + 2);
    out("Fixed value test, TOS ");
    out_hex(tos, 4);
    out(", ROM4 holds ");
    out_hex(*(volatile unsigned short *)ROM4, 4);
    out("\r\n");
    int ranges = RANGES;
    if (*(volatile unsigned long *)ROM4 == CART_MAGIC) {
        gemdrive = 1;
        ranges = RANGES_HIGH;
        out("GEMDRIVE on the cartridge: only FFFF at $FB8000, 0000 at $FB6000\r\n");
    }
    for (int r = 0; r < ranges; r++)
        for (int s = 0; s < STYLES; s++)
            test_range(r, s);
    Super(stack);
    out("Done. Press a key.\r\n");
    if (report >= 0)
        Fclose(report);
    report = -1;
    Cconin();
    return 0;
}
