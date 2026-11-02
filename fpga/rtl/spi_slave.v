// TheHulk - SPI slave towards the ESP32-S3 (SCK, MOSI, MISO, CS)
//
// SPI mode 0, MSB first. CS low frames one command. The protocol is half duplex: the
// ESP32 sends a command and, for reads, the FPGA answers on MISO after a dummy byte.
//
// SCK, MOSI and CS are oversampled with the 48 MHz system clock. MOSI is sampled on
// the rising SCK edge; MISO changes after the falling edge (about 80 ns later), so
// SCK must stay at or below ~5 MHz. CS must stay high for at least ~100 ns between
// commands.
//
// Commands:
//   0x01 WRITE   addr[23:16] addr[15:8] addr[7:0] data...  (byte address, even,
//                big-endian words: first byte = D15..D8, auto increment)
//   0x02 READ    addr[23:16] addr[15:8] addr[7:0] dummy  -> data...
//                (address bit 23 set: the command packet buffer instead of the SPRAM,
//                 word 0 = command, word 1 = payload size in bytes, then the payload)
//   0x03 ID      dummy -> 'H' 'U' 'L' 'K' version
//   0x04 CTRL    ctrl     bit0: ROM emulation enabled
//   0x05 STATUS  dummy -> ctrl rom4_count[31:0] rom3_count[31:0] last_addr[15:0]
//                   addr_changes[31:0] early_count[15:0] pkt_ready[7:0]
//                   pkt_count[15:0] pkt_drops[15:0]
//                   (last_addr = {bank, A15..A1}, bank 1 = /ROM3; addr_changes counts
//                    address bus changes, i.e. ST activity; early_count counts ROM
//                    cycles while ROM emulation was disabled; pkt_count counts the
//                    command packets received, pkt_drops the ones that arrived while
//                    the previous packet was not acknowledged yet)
//   0x06 ACK     the ESP32 has read the packet: release /IRQ, accept the next one

`default_nettype none

module spi_slave (
    input  wire        clk,
    input  wire        cs_n,
    input  wire        sck,
    input  wire        mosi,
    output wire        miso,

    output reg         wr_req,
    output reg  [15:0] wr_addr,
    output reg  [15:0] wr_data,
    output reg         rd_req,
    output reg  [15:0] rd_addr,
    output reg         rd_pkt,      // rd_addr is in the command packet buffer
    input  wire [15:0] rd_data,

    output wire        rom_enable,
    output reg         pkt_ack,

    input  wire [31:0] rom4_count,
    input  wire [31:0] rom3_count,
    input  wire [15:0] last_addr,
    input  wire [31:0] addr_changes,
    input  wire [15:0] early_count,
    input  wire        pkt_ready,
    input  wire [15:0] pkt_count,
    input  wire [15:0] pkt_drops
);
    localparam [7:0] VERSION = 8'h0d;

    localparam CMD_WRITE  = 8'h01;
    localparam CMD_READ   = 8'h02;
    localparam CMD_ID     = 8'h03;
    localparam CMD_CTRL   = 8'h04;
    localparam CMD_STATUS = 8'h05;
    localparam CMD_ACK    = 8'h06;

    localparam S_CMD    = 4'd0;
    localparam S_ADDR   = 4'd1;
    localparam S_WDATA  = 4'd2;
    localparam S_DUMMY  = 4'd3;    // READ: dummy byte, then data
    localparam S_RDATA  = 4'd4;
    localparam S_PRE    = 4'd5;    // ID / STATUS: dummy byte, then stream
    localparam S_STREAM = 4'd6;
    localparam S_CTRL   = 4'd7;
    localparam S_IGNORE = 4'd8;


    // Synchronisers: SCK and MOSI get the same delay so their relation is kept.
    reg [2:0] sck_s  = 3'b000;
    reg [1:0] mosi_s = 2'b00;
    reg [1:0] cs_s   = 2'b11;

    always @(posedge clk) begin
        sck_s  <= {sck_s[1:0], sck};
        mosi_s <= {mosi_s[0], mosi};
        cs_s   <= {cs_s[0], cs_n};
    end

    wire sck_rise = sck_s[2:1] == 2'b01;
    wire sck_fall = sck_s[2:1] == 2'b10;
    wire bit_in   = mosi_s[1];

    // CS high (inactive) ends a command and holds the slave in its idle state
    wire frame_end = cs_s[1];

    reg [2:0]  bitcnt = 3'd0;
    reg [6:0]  rx_sr;
    reg [7:0]  tx_sr = 8'h00;
    reg [7:0]  tx_next;         // next byte, moved into tx_sr on the falling edge
    reg [3:0]  state = S_CMD;
    reg        cmd_rd;
    reg [1:0]  cnt;
    reg [23:0] addr;
    reg        phase;
    reg [7:0]  hi_byte;
    reg [7:0]  ctrl = 8'h00;
    reg [167:0] stream;         // bytes still to be sent in S_STREAM, MSB first

    reg        byte_stb;        // one clock after the 8th rising SCK edge
    reg [7:0]  byte_rx;
    reg        is_wr, is_rd, is_id, is_status, is_ctrl, is_ack;   // decoded byte_rx
    reg        is_stream;                                       // is_id | is_status

    assign miso       = tx_sr[7];
    assign rom_enable = ctrl[0];


    // Stage 1: bit level shifting
    always @(posedge clk) begin
        byte_stb <= 1'b0;
        if (frame_end) begin
            bitcnt <= 3'd0;
        end else if (sck_rise) begin
            rx_sr    <= {rx_sr[5:0], bit_in};
            bitcnt   <= bitcnt + 1'b1;
            byte_rx  <= {rx_sr, bit_in};
            byte_stb <= bitcnt == 3'd7;
            is_wr     <= {rx_sr, bit_in} == CMD_WRITE;
            is_rd     <= {rx_sr, bit_in} == CMD_READ;
            is_id     <= {rx_sr, bit_in} == CMD_ID;
            is_status <= {rx_sr, bit_in} == CMD_STATUS;
            is_ctrl   <= {rx_sr, bit_in} == CMD_CTRL;
            is_ack    <= {rx_sr, bit_in} == CMD_ACK;
            is_stream <= {rx_sr, bit_in} == CMD_ID || {rx_sr, bit_in} == CMD_STATUS;
        end
    end

    // Decoded state, registered: the state only changes on byte_stb, at least 8 SCK
    // clocks apart, so these flags are always settled when the next byte arrives.
    reg st_cmd, st_addr, st_wdata, st_dummy, st_rdata, st_pre, st_stream, st_ctrl;
    reg addr_last;
    always @(posedge clk) begin
        st_cmd    <= state == S_CMD;
        st_addr   <= state == S_ADDR;
        st_wdata  <= state == S_WDATA;
        st_dummy  <= state == S_DUMMY;
        st_rdata  <= state == S_RDATA;
        st_pre    <= state == S_PRE;
        st_stream <= state == S_STREAM;
        st_ctrl   <= state == S_CTRL;
        addr_last <= cnt == 2'd2;
    end

    // Stage 2: byte level protocol. The byte to send next is prepared in tx_next one
    // clock after the last rising edge of a byte, and moved into tx_sr on the falling
    // edge that follows, so MISO only ever changes after a falling edge.
    always @(posedge clk) begin
        wr_req  <= 1'b0;
        rd_req  <= 1'b0;
        pkt_ack <= 1'b0;

        if (frame_end) begin
            state      <= S_CMD;
            tx_sr      <= 8'h00;
            tx_next    <= 8'h00;
        end else begin
            if (sck_fall)
                tx_sr <= (bitcnt == 3'd0) ? tx_next : {tx_sr[6:0], 1'b0};

            if (byte_stb) begin
                if (st_cmd) begin
                    cmd_rd <= is_rd;
                    cnt <= 2'd0;
                    if (is_wr | is_rd) begin
                        state <= S_ADDR;
                    end else if (is_ctrl) begin
                        state <= S_CTRL;
                    end else if (is_stream) begin
                        tx_next <= is_id ? "H" : ctrl;
                        state   <= S_PRE;
                    end else if (is_ack) begin
                        pkt_ack <= 1'b1;
                        state   <= S_IGNORE;
                    end else begin
                        state <= S_IGNORE;
                    end
                end

                if (st_addr) begin
                    addr <= {addr[15:0], byte_rx};
                    cnt  <= cnt + 1'b1;
                    if (addr_last) begin
                        phase <= 1'b0;
                        if (cmd_rd) begin
                            rd_addr <= {addr[15:0], byte_rx} >> 1;
                            rd_pkt  <= addr[15];
                            rd_req  <= 1'b1;
                            state   <= S_DUMMY;
                        end else begin
                            wr_addr <= {addr[15:0], byte_rx} >> 1;
                            state   <= S_WDATA;
                        end
                    end
                end

                if (st_wdata) begin
                    phase <= ~phase;
                    if (!phase) begin
                        hi_byte <= byte_rx;
                    end else begin
                        wr_data <= {hi_byte, byte_rx};
                        wr_req  <= 1'b1;
                    end
                end

                if (st_dummy) begin
                    tx_next    <= rd_data[15:8];
                    phase      <= 1'b1;
                    state      <= S_RDATA;
                end

                if (st_rdata) begin
                    phase <= ~phase;
                    if (phase) begin
                        tx_next <= rd_data[7:0];
                        rd_addr <= rd_addr + 1'b1;
                        rd_req  <= 1'b1;
                    end else begin
                        tx_next <= rd_data[15:8];
                    end
                end

                if (st_pre) begin
                    // tx_next already holds the first byte
                    state      <= S_STREAM;
                end

                if (st_stream)
                    tx_next <= stream[167:160];

                if (st_ctrl) begin
                    ctrl  <= byte_rx;
                    state <= S_IGNORE;
                end

            end
        end

        // The ID / STATUS stream: loaded with the command byte, shifted per byte sent
        if (!frame_end && byte_stb) begin
            if (st_cmd && is_stream)
                stream <= is_id ? {"ULK", VERSION, 136'h0}
                                : {rom4_count, rom3_count, last_addr, addr_changes, early_count,
                                   7'h00, pkt_ready, pkt_count, pkt_drops};
            else if (st_stream)
                stream <= {stream[159:0], 8'h00};
        end

        // Advance the write address after the word has been written
        if (wr_req)
            wr_addr <= wr_addr + 1'b1;
    end

endmodule

`default_nettype wire
