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
// Command channel (SidecarTridge protocol, as in SideTNFS tprotocol.c): the Atari
// sends 16-bit words as byte reads of 0xFB0000 + word, so the word is {A15..A1, A0},
// with A0 = /UDS high (odd byte, /LDS). A packet is 0xABCD, command, payload size in
// bytes, then the payload words. The decoder restarts when a packet is not complete
// 10 ms after its header (the Atari polls the ROM3 area while it waits for the
// answer, about a million reads per second). A complete packet is kept in EBR, /IRQ
// goes low and further packets are dropped until the ESP32 acknowledges it.
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
    // /IRQ to the ESP32: a command packet is waiting. Open drain on the RGB2 pad.
    // ------------------------------------------------------------------
    reg  pkt_ready = 1'b0;
    wire irq_level = ~pkt_ready;
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
    reg        uds_q;
    reg [12:0] a_d;
    reg        a_changed;
    reg [1:0]  rom3_s = 2'b11;
    reg [1:0]  rom4_s = 2'b11;
    reg        rom3_d = 1'b1;
    reg        rom4_d = 1'b1;

    always @(posedge clk) begin
        a_q    <= a;
        uds_q  <= uds_n;
        rom3_s <= {rom3_s[0], rom3_n};
        rom4_s <= {rom4_s[0], rom4_n};
        rom3_d <= rom3_s[1];
        rom4_d <= rom4_s[1];
    end

    // Start of a /ROM4 or /ROM3 cycle (address and /UDS are stable by then)
    wire rom4_start = rom4_d & ~rom4_s[1];
    wire rom3_start = rom3_d & ~rom3_s[1];

    // ------------------------------------------------------------------
    // SPI slave / register interface
    // ------------------------------------------------------------------
    wire        wr_req, rd_req;
    wire [15:0] wr_addr, rd_addr, wr_data;
    wire        rd_pkt;
    reg  [15:0] rd_data;
    wire        rom_enable;
    wire        pkt_ack;
    reg  [15:0] pkt_count = 0;
    reg  [15:0] pkt_drops = 0;

    reg  [31:0] rom4_count   = 0;
    reg  [31:0] rom3_count   = 0;
    reg  [15:0] early_count  = 0;   // ROM cycles while the cartridge was not enabled yet
    reg  [15:0] last_addr    = 0;
    reg  [31:0] addr_changes = 0;   // address bus activity (the ST is running)

    spi_slave spi (
        .clk(clk),
        .cs_n(spi_cs_n), .sck(spi_sck), .mosi(spi_mosi), .miso(spi_miso),
        .wr_req(wr_req), .wr_addr(wr_addr), .wr_data(wr_data),
        .rd_req(rd_req), .rd_addr(rd_addr), .rd_pkt(rd_pkt), .rd_data(rd_data),
        .rom_enable(rom_enable), .pkt_ack(pkt_ack),
        .rom4_count(rom4_count), .rom3_count(rom3_count), .last_addr(last_addr),
        .addr_changes(addr_changes), .early_count(early_count),
        .pkt_ready(pkt_ready), .pkt_count(pkt_count), .pkt_drops(pkt_drops)
    );

    // ------------------------------------------------------------------
    // SPRAM: 4 x 16K x 16
    // ------------------------------------------------------------------
    reg  [13:0] ram_addr;
    reg  [3:0]  ram_we;
    wire [15:0] ram_dout [0:3];

    // Writes are registered once, so WREN comes straight from flip-flops (timing)
    reg         wr_q = 1'b0;
    reg  [15:0] wr_addr_q, wr_data_q;

    always @(posedge clk) begin
        wr_q      <= wr_req;
        wr_addr_q <= wr_addr;
        wr_data_q <= wr_data;
    end

    always @* begin
        ram_we   = 4'b0000;
        ram_addr = a_q[13:0];
        if (wr_q) begin
            ram_addr = wr_addr_q[13:0];
            ram_we[wr_addr_q[15:14]] = 1'b1;
        end else if (rd_req) begin
            ram_addr = rd_addr[13:0];
        end
    end

    genvar i;
    generate
        for (i = 0; i < 4; i = i + 1) begin : g_spram
            SB_SPRAM256KA ram (
                .ADDRESS(ram_addr),
                .DATAIN(wr_data_q),
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
    reg       rd_pending_pkt;
    reg       bus_valid;
    reg [15:0] pkt_rdata;

    always @(posedge clk) begin
        bus_blk    <= {~rom3_s[0], a_q[14]};   // /ROM3 low -> upper 64 KB
        bus_valid  <= ~(wr_q | rd_req);
        rd_pending <= rd_req;
        rd_pending_pkt <= rd_pkt;
        rd_blk     <= rd_addr[15:14];
        if (rd_pending)
            rd_data <= rd_pending_pkt ? pkt_rdata : ram_dout[rd_blk];
    end

    // ------------------------------------------------------------------
    // Command channel: protocol decoder and packet buffer (2048 words of EBR)
    // ------------------------------------------------------------------
    localparam [15:0] PROTOCOL_HEADER = 16'hABCD;
    localparam [18:0] RESTART_CLOCKS  = 19'd480000;   // 10 ms at 48 MHz (9.1-11.1 ms)

    localparam P_HEADER = 2'd0;
    localparam P_CMD    = 2'd1;
    localparam P_SIZE   = 2'd2;
    localparam P_DATA   = 2'd3;

    reg [15:0] pkt_mem [0:2047];
    reg [1:0]  p_state = P_HEADER;
    reg [18:0] p_timer = 0;
    reg [10:0] p_widx;
    reg [15:0] p_left;             // payload bytes still to come (odd sizes: rounded up)
    reg        p_we;
    reg [10:0] p_waddr;
    reg [15:0] p_wdata;

    // The address is stable for several clocks before rom3_start, so the comparisons
    // are registered a clock ahead (timing)
    wire [15:0] rom3_word = {a_q, uds_q};
    reg         hdr_match;
    reg         word_zero;
    reg         p_expired;
    reg         p_left_le2;        // the next payload word is the last one
    reg         p_done;            // the packet is complete
    wire        p_restart = p_state != P_HEADER && p_expired;

    always @(posedge clk) begin
        if (p_we)
            pkt_mem[p_waddr] <= p_wdata;
        pkt_rdata <= pkt_mem[rd_addr[10:0]];
    end

    always @(posedge clk) begin
        p_we      <= 1'b0;
        hdr_match <= rom3_word == PROTOCOL_HEADER;
        word_zero <= rom3_word == 16'd0;
        p_expired <= p_timer == 0;
        p_left_le2 <= p_left <= 16'd2;
        if (p_timer != 0)
            p_timer <= p_timer - 1'b1;
        if (pkt_ack)
            pkt_ready <= 1'b0;

        if (rom3_start) begin
            if (p_state == P_HEADER || p_restart) begin
                p_state <= P_HEADER;
                if (hdr_match) begin
                    if (pkt_ready) begin
                        if (pkt_drops != 16'hffff)
                            pkt_drops <= pkt_drops + 1'b1;
                    end else begin
                        p_state <= P_CMD;
                        p_timer <= RESTART_CLOCKS;
                    end
                end
            end else begin
                p_we    <= 1'b1;
                p_wdata <= rom3_word;
                case (p_state)
                    P_CMD: begin
                        p_waddr <= 11'd0;
                        p_state <= P_SIZE;
                    end
                    P_SIZE: begin
                        p_waddr <= 11'd1;
                        p_widx  <= 11'd2;
                        p_left  <= rom3_word;
                        p_done  <= word_zero;
                        p_state <= P_DATA;
                    end
                    default: begin       // P_DATA
                        p_waddr <= p_widx;
                        if (p_widx != 11'd2047)
                            p_widx <= p_widx + 1'b1;
                        p_left  <= p_left - 16'd2;
                        p_done  <= p_left_le2;
                    end
                endcase
            end
        end

        // Packet complete: no payload, or the last payload word just arrived
        if (p_state == P_DATA && p_done && !rom3_start) begin
            p_state   <= P_HEADER;
            pkt_ready <= 1'b1;
            pkt_count <= pkt_count + 1'b1;
        end
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
        a_changed <= a_d != a_q[12:0];
        if (a_changed)
            addr_changes <= addr_changes + 1'b1;
    end

endmodule

`default_nettype wire
