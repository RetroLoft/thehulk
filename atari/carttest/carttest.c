/* Cartridge read test for TheHulk.
 *
 * Writes a file with known patterns, reads it through GEMDOS and then reads the read buffer
 * of GEMDRIVE on the cartridge ($FB0108) directly, with long, word and byte reads. A wrong
 * word that comes back at the same place in every pass is wrong in the cartridge RAM; one
 * that comes and goes is lost on the bus. The results go to the screen and to CARTTEST.TXT.
 */
#include <osbind.h>

#define BLOCK       16384L
#define WORDS       (BLOCK / 2)
#define PATTERNS    8
#define ROUNDS      3
#define PASSES      8       // the second half after a pause, see direct_reads
#define PAUSE_TICKS 200     // 1 s of the 200 Hz timer
#define STYLES      4
#define MAX_SHOWN   16
#define READ_BUFFER ((const volatile void *)0xFB0108L)
// On a cartridge drive, wherever the program itself is: the test reads through GEMDRIVE
#define DATA_FILE   "N:\\FALCON\\CARTTEST.DAT"
#define REPORT_FILE "N:\\FALCON\\CARTTEST.TXT"

static const char *const style_name[STYLES] = { "Fread", "long ", "word ", "byte " };
static const char *const pattern_name[PATTERNS] = {
    "FFFF      ", "0000 FFFF ", "00 00 FF FF", "FFFF 1x0  ", "0000 1xF  ", "walking 1 ", "walking 0 ", "random    "
};

static unsigned short expect[WORDS];
static unsigned short got[WORDS];
static unsigned char wrong[WORDS];

static unsigned long reads[STYLES];
static unsigned long errors[STYLES];
static unsigned long pattern_errors[PATTERNS];
static unsigned long lost[16], gained[16];
static unsigned long stuck, flaky;
static unsigned long pass_errors[PASSES];
static int shown;
static short report = -1;

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

static void make_pattern(int p)
{
    unsigned short x = 0xACE1;
    for (long i = 0; i < WORDS; i++) {
        unsigned short v = 0;
        switch (p) {
        case 0: v = 0xFFFF; break;
        case 1: v = i & 1 ? 0xFFFF : 0x0000; break;
        case 2: v = i & 2 ? 0xFFFF : 0x0000; break;
        case 3: v = i % 16 ? 0xFFFF : 0x0000; break;
        case 4: v = i % 16 ? 0x0000 : 0xFFFF; break;
        case 5: v = 1 << (i & 15); break;
        case 6: v = ~(1 << (i & 15)); break;
        default:
            x ^= x << 7;
            x ^= x >> 9;
            x ^= x << 8;
            v = x;
            break;
        }
        expect[i] = v;
    }
}

static void read_long(void)
{
    const volatile void *src = READ_BUFFER;
    void *dst = got;
    long n = BLOCK / 4 - 1;
    __asm__ volatile ("1: move.l (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
}

static void read_word(void)
{
    const volatile void *src = READ_BUFFER;
    void *dst = got;
    long n = BLOCK / 2 - 1;
    __asm__ volatile ("1: move.w (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
}

static void read_byte(void)
{
    const volatile void *src = READ_BUFFER;
    void *dst = got;
    long n = BLOCK - 1;
    __asm__ volatile ("1: move.b (%0)+,(%1)+\n dbf %2,1b" : "+a" (src), "+a" (dst), "+d" (n) : : "memory", "cc");
}

static void compare(int style, int pattern, int round)
{
    reads[style] += WORDS;
    for (long i = 0; i < WORDS; i++) {
        unsigned short e = expect[i], g = got[i], d = e ^ g;
        if (!d)
            continue;
        errors[style]++;
        pattern_errors[pattern]++;
        if (style && wrong[i] < 255)
            wrong[i]++;
        for (int b = 0; b < 16; b++)
            if (d >> b & 1) {
                if (e >> b & 1)
                    lost[b]++;
                else
                    gained[b]++;
            }
        if (shown < MAX_SHOWN) {
            shown++;
            out(style_name[style]);
            out(" r");
            out_dec(round);
            out(" ");
            out(pattern_name[pattern]);
            out(" +");
            out_hex(i * 2, 4);
            out(" want ");
            out_hex(e, 4);
            out(" got ");
            out_hex(g, 4);
            out("\r\n");
        }
    }
}

static void direct_reads(int pattern, int round)
{
    int total = PASSES * (STYLES - 1);
    long stack = Super(0L);
    for (long i = 0; i < WORDS; i++)
        wrong[i] = 0;
    for (int pass = 0; pass < PASSES; pass++) {
        if (pass == PASSES / 2) {       // does it depend on the time since the Fread?
            unsigned long start = *(volatile unsigned long *)0x4BAL;
            while (*(volatile unsigned long *)0x4BAL - start < PAUSE_TICKS)
                ;
        }
        unsigned long before = errors[1] + errors[2] + errors[3];
        read_long();
        compare(1, pattern, round);
        read_word();
        compare(2, pattern, round);
        read_byte();
        compare(3, pattern, round);
        pass_errors[pass] += errors[1] + errors[2] + errors[3] - before;
    }
    Super(stack);
    for (long i = 0; i < WORDS; i++) {
        if (wrong[i] == total)
            stuck++;
        else if (wrong[i])
            flaky++;
    }
}

static int make_file(void)
{
    long h = Fcreate(DATA_FILE, 0);
    if (h < 0) {
        out("Cannot create CARTTEST.DAT: this drive must be writable\r\n");
        return 0;
    }
    for (int p = 0; p < PATTERNS; p++) {
        make_pattern(p);
        if (Fwrite(h, BLOCK, expect) != BLOCK) {
            out("Cannot write CARTTEST.DAT\r\n");
            Fclose(h);
            return 0;
        }
    }
    Fclose(h);
    return 1;
}

static void clear_counts(void)
{
    for (int i = 0; i < STYLES; i++)
        reads[i] = errors[i] = 0;
    for (int i = 0; i < PATTERNS; i++)
        pattern_errors[i] = 0;
    for (int i = 0; i < 16; i++)
        lost[i] = gained[i] = 0;
    stuck = flaky = 0;
    for (int i = 0; i < PASSES; i++)
        pass_errors[i] = 0;
    shown = 0;
}

static void report_counts(void)
{
    out("\r\nReads (words) and wrong words\r\n");
    for (int s = 0; s < STYLES; s++) {
        out(style_name[s]);
        out("  ");
        out_dec(reads[s]);
        out("  wrong ");
        out_dec(errors[s]);
        out("\r\n");
    }
    out("Wrong words per pattern\r\n");
    for (int p = 0; p < PATTERNS; p++) {
        out(pattern_name[p]);
        out("  ");
        out_dec(pattern_errors[p]);
        out("\r\n");
    }
    out("Bits D15..D0, lost (1 to 0)\r\n");
    for (int b = 15; b >= 0; b--) {
        out_dec(lost[b]);
        out(b ? " " : "\r\n");
    }
    out("Bits D15..D0, gained (0 to 1)\r\n");
    for (int b = 15; b >= 0; b--) {
        out_dec(gained[b]);
        out(b ? " " : "\r\n");
    }
    out("Wrong words per direct pass, after the Fread; 1 s pause before pass 5:\r\n");
    for (int i = 0; i < PASSES; i++) {
        out_dec(pass_errors[i]);
        out(i < PASSES - 1 ? " " : "\r\n");
    }
    out("Direct reads: wrong in every pass ");
    out_dec(stuck);
    out(", now and then ");
    out_dec(flaky);
    out("\r\n");
}

static void run_test(void)
{
    long f = Fopen(DATA_FILE, 0);
    if (f < 0) {
        out("Cannot open CARTTEST.DAT\r\n");
        return;
    }
    clear_counts();
    for (int round = 1; round <= ROUNDS; round++) {
        out("Round ");
        out_dec(round);
        out(" of ");
        out_dec(ROUNDS);
        out("\r\n");
        for (int p = 0; p < PATTERNS; p++) {
            make_pattern(p);
            Fseek(p * BLOCK, f, 0);
            long n = Fread(f, BLOCK, got);
            if (n != BLOCK) {
                out("Fread returned ");
                out_dec(n < 0 ? -n : n);
                out("\r\n");
                continue;
            }
            compare(0, p, round);
            direct_reads(p, round);
        }
    }
    Fclose(f);
    report_counts();
}

#ifdef SCOPE
/* For the oscilloscope: reads one pattern from the cartridge over and over. Space: the next
 * pattern. Esc: stop. Needs the CARTTEST.DAT that CARTTEST.PRG left behind. */
int main(void)
{
    long f = Fopen(DATA_FILE, 0);
    if (f < 0) {
        out("Run CARTTEST.PRG first, CARTTEST.DAT is missing\r\n");
        Cconin();
        return 1;
    }
    int p = 0;
    for (;;) {
        Fseek(p * BLOCK, f, 0);
        Fread(f, BLOCK, got);
        out(pattern_name[p]);
        out("\r\n");
        long stack = Super(0L);
        long key = 0;
        while (!Bconstat(2))
            read_long();
        key = Bconin(2);
        Super(stack);
        if ((key & 0xFF) == 27)
            break;
        p = (p + 1) % PATTERNS;
    }
    Fclose(f);
    return 0;
}
#else
int main(void)
{
    long h = Fcreate(REPORT_FILE, 0);
    if (h >= 0)
        report = h;
    out("TheHulk cartridge read test\r\n");
    long stack = Super(0L);
    unsigned long sysbase = *(volatile unsigned long *)0x4F2L;
    unsigned short tos = *(volatile unsigned short *)(sysbase + 2);
    Super(stack);
    out("TOS ");
    out_hex(tos, 4);
    out("\r\nWriting the test file...\r\n");
    if (make_file()) {
        run_test();
    }
    out("Done, see N:\\FALCON\\CARTTEST.TXT. Press a key.\r\n");
    if (report >= 0)
        Fclose(report);
    report = -1;
    Cconin();
    return 0;
}
#endif
