// TheHulk - Atari ST cartridge emulator, iCE40UP5K top level (board v2)
//
// Memory map (128 KB, held in the four SPRAM blocks of the UP5K):
//   word address {bank, A15..A1}, bank 0 = /ROM4 (0xFA0000), bank 1 = /ROM3 (0xFB0000)
//   SPRAM block = word_addr[15:14], SPRAM address = word_addr[13:0]
//
// Bus side: the address bus is always visible through U8. The data buffer U9 is
// enabled in hardware whenever /ROM3 or /ROM4 is low, so we simply keep the data
// pins updated with the word at the current address (latency ~3 clocks @ 48 MHz).
//
// ESP32 side: SPI slave (SCK, MOSI, MISO, CS), see spi_slave.v.
//
// Pins: see thehulk.pcf. A14 and A15 are on the RGB0/RGB1 pads (inputs, no internal
// pull-up), /IRQ on RGB2 (open drain, pull-up on the board).

`default_nettype none

module top (
    input  wire [14:0] a,        // A1..A15
    input  wire        uds_n,
    input  wire        rom3_n,
    input  wire        rom4_n,
    output wire [15:0] d,

    input  wire        spi_cs_n,
    input  wire        spi_sck,
    input  wire        spi_mosi,
    output wire        spi_miso,

    output wire        irq_n
`ifdef SIM
    , input wire       sim_clk
`endif
);
    // ------------------------------------------------------------------
    // Clock: internal 48 MHz oscillator (there is no external clock)
    // ------------------------------------------------------------------
    wire clk;
`ifdef SIM
    assign clk = sim_clk;
`else
    SB_HFOSC #(.CLKHF_DIV("0b00")) hfosc (
        .CLKHFPU(1'b1),
        .CLKHFEN(1'b1),
        .CLKHF(clk)
    );
`endif

    // ------------------------------------------------------------------
    // /IRQ to the ESP32 (not used yet): open drain on the RGB2 pad
    // ------------------------------------------------------------------
    wire irq_level = 1'b1;
`ifdef SIM
    assign irq_n = irq_level;
`else
    SB_IO #(.PIN_TYPE(6'b1010_01)) irq_io (
        .PACKAGE_PIN(irq_n),
        .OUTPUT_ENABLE(~irq_level),
        .D_OUT_0(1'b0)
    );
`endif

    // ------------------------------------------------------------------
    // Atari bus input registers
    // ------------------------------------------------------------------
    reg [14:0] a_q;
    reg [12:0] a_d;
    reg [1:0]  rom3_s = 2'b11;
    reg [1:0]  rom4_s = 2'b11;
    reg        rom3_d = 1'b1;
    reg        rom4_d = 1'b1;

    always @(posedge clk) begin
        a_q    <= a;
        rom3_s <= {rom3_s[0], rom3_n};
        rom4_s <= {rom4_s[0], rom4_n};
        rom3_d <= rom3_s[1];
        rom4_d <= rom4_s[1];
    end

    // ------------------------------------------------------------------
    // SPI slave / register interface
    // ------------------------------------------------------------------
    wire        wr_req, rd_req;
    wire [15:0] wr_addr, rd_addr, wr_data;
    reg  [15:0] rd_data;
    wire        rom_enable;

    reg  [31:0] rom4_count   = 0;
    reg  [31:0] rom3_count   = 0;
    reg  [15:0] early_count  = 0;   // ROM cycles while the cartridge was not enabled yet
    reg  [15:0] last_addr    = 0;
    reg  [31:0] addr_changes = 0;   // address bus activity (the ST is running)

    spi_slave spi (
        .clk(clk),
        .cs_n(spi_cs_n), .sck(spi_sck), .mosi(spi_mosi), .miso(spi_miso),
        .wr_req(wr_req), .wr_addr(wr_addr), .wr_data(wr_data),
        .rd_req(rd_req), .rd_addr(rd_addr), .rd_data(rd_data),
        .rom_enable(rom_enable),
        .rom4_count(rom4_count), .rom3_count(rom3_count), .last_addr(last_addr),
        .addr_changes(addr_changes), .early_count(early_count)
    );

    // ------------------------------------------------------------------
    // SPRAM: 4 x 16K x 16
    // ------------------------------------------------------------------
    reg  [13:0] ram_addr;
    reg  [3:0]  ram_we;
    wire [15:0] ram_dout [0:3];

    always @* begin
        ram_we   = 4'b0000;
        ram_addr = a_q[13:0];
        if (wr_req) begin
            ram_addr = wr_addr[13:0];
            ram_we[wr_addr[15:14]] = 1'b1;
        end else if (rd_req) begin
            ram_addr = rd_addr[13:0];
        end
    end

    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : g_spram
            SB_SPRAM256KA ram (
                .ADDRESS(ram_addr),
                .DATAIN(wr_data),
                .MASKWREN(4'b1111),
                .WREN(ram_we[i]),
                .CHIPSELECT(1'b1),
                .CLOCK(clk),
                .STANDBY(1'b0),
                .SLEEP(1'b0),
                .POWEROFF(1'b1),
                .DATAOUT(ram_dout[i])
            );
        end
    endgenerate

    // Which block the SPRAM output belongs to (one clock after the address)
    reg [1:0] bus_blk;
    reg [1:0] rd_blk;
    reg       rd_pending = 1'b0;
    reg       bus_valid;

    always @(posedge clk) begin
        bus_blk    <= {~rom3_s[0], a_q[14]};   // /ROM3 low -> upper 64 KB
        bus_valid  <= ~(wr_req | rd_req);
        rd_pending <= rd_req;
        rd_blk     <= rd_addr[15:14];
        if (rd_pending)
            rd_data <= ram_dout[rd_blk];
    end

    // ------------------------------------------------------------------
    // Data bus output
    // ------------------------------------------------------------------
    reg [15:0] d_q = 16'hffff;

    always @(posedge clk)
        if (!rom_enable)
            d_q <= 16'hffff;
        else if (bus_valid)
            d_q <= ram_dout[bus_blk];

    assign d = d_q;

    // ------------------------------------------------------------------
    // Counters (read through the SPI status command)
    // ------------------------------------------------------------------
    wire rom4_start = rom4_d & ~rom4_s[1];
    wire rom3_start = rom3_d & ~rom3_s[1];

    always @(posedge clk) begin
        if (rom4_start)
            rom4_count <= rom4_count + 1'b1;
        if (rom3_start)
            rom3_count <= rom3_count + 1'b1;
        if (rom4_start | rom3_start)
            last_addr <= {~rom3_s[1], a_q[14:0]};
        if ((rom4_start | rom3_start) && !rom_enable && early_count != 16'hffff)
            early_count <= early_count + 1'b1;
        // A14/A15 float while the ST is off (RGB pads, no pull-up): only A1..A13 here
        a_d <= a_q[12:0];
        if (a_d != a_q[12:0])
            addr_changes <= addr_changes + 1'b1;
    end

    wire _unused = &{uds_n, 1'b0};

endmodule

`default_nettype wire
