// CXXRTL testbench for TheHulk gateware.
// Loads a ROM image over the SPI link (SCK, MOSI, MISO, CS), reads it back, then
// emulates Atari ST cartridge reads. The SPI master models the ESP32: half-duplex
// commands, one command per CS-low frame.
//
// Environment: CLK_MHZ (FPGA clock), SPI_MHZ, SAMPLE_LATE=1 (master samples just
// before the falling edge instead of at the rising edge).
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "top_sim.h"

using namespace cxxrtl_design;

static p_top top;
static double t_ns = 0, next_clk = 0;
static double CLK_HALF = 1000.0 / 48.0 / 2.0;
static bool clk = false;

static void advance(double ns)
{
    double end = t_ns + ns;
    top.step();
    while (next_clk <= end) {
        t_ns = next_clk;
        clk = !clk;
        top.p_sim__clk.set<bool>(clk);
        top.step();
        next_clk += CLK_HALF;
    }
    t_ns = end;
}

// --- ESP32 model: SPI mode 0, half-duplex commands framed by CS -----------
static double spi_half = 125;            // 4 MHz
static bool sample_late = false;
static double gap_ns = 300;              // CS high between commands

static void spi_bit_out(int bit)
{
    top.p_spi__mosi.set<bool>(bit);
    advance(spi_half);
    top.p_spi__sck.set<bool>(true);
    advance(spi_half);
    top.p_spi__sck.set<bool>(false);
}

static int spi_bit_in(void)
{
    top.p_spi__mosi.set<bool>(false);
    advance(spi_half);
    bool bit = top.p_spi__miso.get<bool>();      // sample at the rising edge
    top.p_spi__sck.set<bool>(true);
    advance(spi_half);
    if (sample_late)
        bit = top.p_spi__miso.get<bool>();       // sample just before the falling edge
    top.p_spi__sck.set<bool>(false);
    return bit;
}

// One command: CS low, tx bytes, then rx_len bytes, CS high
static std::vector<uint8_t> spi_xfer(const std::vector<uint8_t> &tx, size_t rx_len)
{
    std::vector<uint8_t> rx;
    top.p_spi__cs__n.set<bool>(false);
    advance(100);
    for (uint8_t b : tx)
        for (int i = 7; i >= 0; i--)
            spi_bit_out((b >> i) & 1);
    for (size_t n = 0; n < rx_len; n++) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++)
            v = (v << 1) | spi_bit_in();
        rx.push_back(v);
    }
    advance(50);
    top.p_spi__cs__n.set<bool>(true);
    advance(gap_ns);
    return rx;
}

static std::vector<uint8_t> cmd_id(void)        { return spi_xfer({0x03, 0x00}, 5); }
static void cmd_ctrl(uint8_t v)                 { spi_xfer({0x04, v}, 0); }
static std::vector<uint8_t> cmd_status(void)    { return spi_xfer({0x05, 0x00}, 22); }
static void cmd_ack(void)                       { spi_xfer({0x06}, 0); }

static void cmd_write(uint32_t addr, const uint8_t *data, size_t len)
{
    std::vector<uint8_t> tx = { 0x01, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    tx.insert(tx.end(), data, data + len);
    spi_xfer(tx, 0);
}

static std::vector<uint8_t> cmd_read(uint32_t addr, size_t len)
{
    return spi_xfer({ 0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 0x00 }, len);
}

static bool id_ok(const std::vector<uint8_t> &id)
{
    return id.size() == 5 && id[0] == 'H' && id[1] == 'U' && id[2] == 'L' && id[3] == 'K';
}

// --- Atari bus -----------------------------------------------------------
// Returns the earliest time (ns after /ROMx low) from which the data stayed correct.
static int bus_read(const std::vector<uint8_t> &img, uint32_t byte_addr, uint16_t &got)
{
    bool rom3 = byte_addr >= 0x10000;
    uint16_t a = (byte_addr >> 1) & 0x7fff;
    uint16_t expect = (img[byte_addr & ~1u] << 8) | img[(byte_addr & ~1u) + 1];

    top.p_a.set<uint16_t>(a);
    advance(30 + rand() % 20);           // address valid before /AS -> /ROMx
    if (rom3) top.p_rom3__n.set<bool>(false); else top.p_rom4__n.set<bool>(false);

    int first_ok = -1;
    for (int ns = 0; ns <= 300; ns += 2) {
        advance(2);
        got = top.p_d.get<uint16_t>();
        if (got == expect) { if (first_ok < 0) first_ok = ns; }
        else first_ok = -1;
    }
    top.p_rom3__n.set<bool>(true);
    top.p_rom4__n.set<bool>(true);
    // Next address change happens shortly after; drive garbage on the bus meanwhile
    advance(20);
    top.p_a.set<uint16_t>(rand() & 0x7fff);
    advance(150 + rand() % 100);
    return first_ok;
}

// --- Command channel: the Atari sends data as byte reads of 0xFB0000 + offset ---
static bool falcon = false;              // the Falcon drives /UDS low on every read

static void atari_read(uint16_t offset)
{
    top.p_a.set<uint16_t>(offset >> 1);
    top.p_uds__n.set<bool>(falcon ? false : (offset & 1));   // ST: odd byte = /LDS only
    advance(30 + rand() % 20);
    top.p_rom3__n.set<bool>(false);
    advance(250);
    top.p_rom3__n.set<bool>(true);
    advance(20);
    top.p_a.set<uint16_t>(rand() & 0x7fff);
    advance(100 + rand() % 100);
}

// The Atari polls the token address while it waits: reads that are not 0xABCD
static void atari_poll(int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t w = rand();
        atari_read((w & 0xfffe) == 0xabcc ? 0x1234 : w);
    }
}

// A word as two reads on even addresses, high byte first
static void atari_word(uint16_t w)
{
    atari_read((w >> 8) << 1);
    atari_read((w & 0xff) << 1);
}

static void atari_packet(uint16_t cmd, const std::vector<uint16_t> &payload, uint16_t size)
{
    atari_read(0xabcd);
    atari_word(cmd);
    atari_word(size);
    for (uint16_t w : payload)
        atari_word(w);
}

struct status_t { uint32_t rom4, rom3, act; unsigned early, ready, count, drops; uint8_t ctrl; };

static status_t get_status(void)
{
    std::vector<uint8_t> st = cmd_status();
    status_t s;
    s.ctrl  = st[0];
    s.rom4  = (st[1] << 24) | (st[2] << 16) | (st[3] << 8) | st[4];
    s.rom3  = (st[5] << 24) | (st[6] << 16) | (st[7] << 8) | st[8];
    s.act   = (st[11] << 24) | (st[12] << 16) | (st[13] << 8) | st[14];
    s.early = (st[15] << 8) | st[16];
    s.ready = st[17];
    s.count = (st[18] << 8) | st[19];
    s.drops = (st[20] << 8) | st[21];
    return s;
}

// Reads the packet buffer and compares it with the expected packet
static int check_packet(const char *name, uint16_t cmd, uint16_t size, const std::vector<uint16_t> &payload)
{
    int err = 0;
    size_t words = 2 + payload.size();
    std::vector<uint8_t> rb = cmd_read(0x800000, words * 2);
    std::vector<uint16_t> expect = { cmd, size };
    expect.insert(expect.end(), payload.begin(), payload.end());
    for (size_t i = 0; i < words; i++) {
        uint16_t got = (rb[2 * i] << 8) | rb[2 * i + 1];
        if (got != expect[i] && err++ < 5)
            printf("%s: packet word %zu: %04x != %04x\n", name, i, got, expect[i]);
    }
    return err;
}

static int test_command_channel(void)
{
    int errors = 0;
    status_t s = get_status();
    unsigned count0 = s.count, drops0 = s.drops;
    if (!top.p_irq__n.get<bool>() || s.ready) { printf("/IRQ low before any packet\n"); errors++; }

    // A packet with token and parameters, surrounded by polling reads
    atari_poll(20);
    std::vector<uint16_t> p1 = { 0x5678, 0x1234, 0x0000, 0xffff, 0x00a5, 0xabcd };
    atari_packet(0x0105, p1, 12);
    if (top.p_irq__n.get<bool>()) { printf("/IRQ not low after a packet\n"); errors++; }
    atari_poll(50);                              // the Atari waits for the token
    errors += check_packet("p1", 0x0105, 12, p1);

    // A second packet before the acknowledge is dropped
    atari_packet(0x0007, { 0x1111, 0x2222 }, 4);
    s = get_status();
    if (!s.ready || s.count != count0 + 1 || s.drops != drops0 + 1) {
        printf("before ack: ready %u count %u drops %u\n", s.ready, s.count - count0, s.drops - drops0);
        errors++;
    }
    errors += check_packet("p1 kept", 0x0105, 12, p1);

    cmd_ack();
    if (!top.p_irq__n.get<bool>()) { printf("/IRQ still low after the acknowledge\n"); errors++; }

    // No payload, then an odd payload size (rounded up to whole words)
    atari_packet(0x0002, {}, 0);
    errors += check_packet("empty", 0x0002, 0, {});
    cmd_ack();
    std::vector<uint16_t> p3 = { 0xaaaa, 0xbbbb, 0xcc00 };
    atari_packet(0x0003, p3, 5);
    errors += check_packet("odd", 0x0003, 5, p3);
    cmd_ack();

    // A long packet (1024 payload bytes)
    std::vector<uint16_t> p4;
    for (int i = 0; i < 512; i++) p4.push_back(rand());
    atari_packet(0x0004, p4, 1024);
    errors += check_packet("long", 0x0004, 1024, p4);
    cmd_ack();

    // An incomplete packet: the decoder restarts 10 ms after its header
    atari_packet(0x0099, { 0x1234 }, 8);         // 3 of 4 payload words missing
    advance(12e6);                               // 10 ms +-10 % (HFOSC tolerance)
    std::vector<uint16_t> p5 = { 0x4242, 0x4343 };
    atari_packet(0x0005, p5, 4);
    s = get_status();
    if (!s.ready) { printf("no packet after the restart\n"); errors++; }
    errors += check_packet("after restart", 0x0005, 4, p5);
    cmd_ack();

    s = get_status();
    if (s.count != count0 + 5 || s.ready) {
        printf("end: count %u (expected 5), ready %u\n", s.count - count0, s.ready);
        errors++;
    }
    printf("command channel: %u packets, %u dropped, errors: %d\n", s.count - count0, s.drops - drops0, errors);
    return errors;
}

int main(int argc, char **argv)
{
    if (getenv("CLK_MHZ")) CLK_HALF = 1000.0 / atof(getenv("CLK_MHZ")) / 2.0;
    if (getenv("SAMPLE_LATE") && *getenv("SAMPLE_LATE")) sample_late = true;
    if (getenv("SPI_MHZ")) spi_half = 1000.0 / atof(getenv("SPI_MHZ")) / 2.0;
    printf("clock: %.1f MHz, SPI %.1f MHz, sample %s\n", 1000.0 / CLK_HALF / 2.0,
           1000.0 / spi_half / 2.0, sample_late ? "late" : "at rising edge");

    // Test image: a file given on the command line, or pseudo-random data
    std::vector<uint8_t> img(131072, 0xff);
    size_t n = img.size();
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        n = fread(img.data(), 1, img.size(), f);
        fclose(f);
    } else {
        srand(1);
        for (auto &b : img) b = rand();
    }
    printf("image: %zu bytes\n", n);

    int errors = 0;
    top.p_spi__sck.set<bool>(false);
    top.p_spi__cs__n.set<bool>(true);
    top.p_rom3__n.set<bool>(true);
    top.p_rom4__n.set<bool>(true);
    top.p_uds__n.set<bool>(true);
    advance(3000);

    // ID
    std::vector<uint8_t> id = cmd_id();
    printf("ID: %c%c%c%c v%d\n", id[0], id[1], id[2], id[3], id[4]);
    if (!id_ok(id)) errors++;

    // Framing robustness: an aborted command, a stray SCK pulse with CS high, a read
    // cut off before the dummy byte, then ID again
    spi_xfer({0x01, 0x12}, 0);
    top.p_spi__sck.set<bool>(true); advance(100); top.p_spi__sck.set<bool>(false); advance(gap_ns);
    spi_xfer({0x02, 0x00, 0x00}, 3);
    id = cmd_id();
    if (!id_ok(id)) { printf("ID after aborted commands failed\n"); errors++; }

    // Data bus must read 0xFFFF while disabled
    uint16_t got;
    bus_read(img, 0, got);
    printf("disabled bus read: %04x\n", got);
    if (got != 0xffff) errors++;

    // Write image in 4 KB commands (as the ESP32 does)
    for (size_t off = 0; off < img.size(); off += 4096)
        cmd_write(off, &img[off], 4096);
    printf("image written (t=%.1f ms)\n", t_ns / 1e6);

    // Read back everything in 4 KB commands
    int rb_err = 0;
    for (size_t off = 0; off < img.size(); off += 4096) {
        std::vector<uint8_t> rb = cmd_read(off, 4096);
        for (size_t i = 0; i < 4096; i++)
            if (rb[i] != img[off + i] && rb_err++ < 10)
                printf("readback %05zx: %02x != %02x\n", off + i, rb[i], img[off + i]);
    }
    printf("readback errors: %d\n", rb_err);
    errors += rb_err;

    // Read back at an odd starting word in bank 1
    std::vector<uint8_t> rb2 = cmd_read(0x12346, 16);
    for (int i = 0; i < 16; i++)
        if (rb2[i] != img[0x12346 + i]) {
            printf("readback2 %x: %02x != %02x\n", 0x12346 + i, rb2[i], img[0x12346 + i]);
            errors++;
        }

    // Enable ROM emulation
    cmd_ctrl(0x01);

    // Bus reads, with SPI status polls in between (as the ESP32 does while the ST runs)
    int worst = 0, bus_err = 0;
    const int N = 4000;
    for (int i = 0; i < N; i++) {
        uint32_t addr = (i < 64) ? (uint32_t)i * 2 : (uint32_t)(rand() % 131072) & ~1u;
        int ok = bus_read(img, addr, got);
        if (ok < 0) {
            if (bus_err++ < 10) printf("bus read %05x failed: got %04x\n", addr, got);
        } else if (ok > worst) worst = ok;
        if (i % 500 == 0 && !id_ok(cmd_id())) { printf("ID during bus reads failed\n"); errors++; }
    }
    printf("bus reads: %d, errors: %d, worst latency after /ROMx low: %d ns\n", N, bus_err, worst);
    errors += bus_err;

    // Status
    status_t s = get_status();
    printf("status: ctrl=%02x rom4=%u rom3=%u addr_changes=%u early=%u\n",
           s.ctrl, s.rom4, s.rom3, s.act, s.early);
    if (s.ctrl != 0x01) { printf("ctrl readback wrong\n"); errors++; }
    if (s.rom4 + s.rom3 != N + 1) { printf("counter mismatch\n"); errors++; }
    if (s.act < N) { printf("address activity counter too low\n"); errors++; }
    if (s.early != 1) { printf("expected 1 early ROM cycle (the disabled bus read)\n"); errors++; }

    errors += test_command_channel();
    falcon = true;
    printf("again with /UDS low on every read (Falcon):\n");
    errors += test_command_channel();

    // The cartridge data must still be correct after the command traffic
    int bus_err2 = 0;
    for (int i = 0; i < 500; i++) {
        uint32_t addr = (uint32_t)(rand() % 131072) & ~1u;
        if (bus_read(img, addr, got) < 0 && bus_err2++ < 5)
            printf("bus read %05x after commands failed: got %04x\n", addr, got);
    }
    errors += bus_err2;

    printf(errors ? "FAIL (%d errors)\n" : "PASS\n", errors);
    return errors ? 1 : 0;
}
