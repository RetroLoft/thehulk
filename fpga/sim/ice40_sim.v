// Simplified simulation models of the iCE40 primitives used by TheHulk (CXXRTL)

module SB_SPRAM256KA (
    input  [13:0] ADDRESS,
    input  [15:0] DATAIN,
    input  [3:0]  MASKWREN,
    input         WREN, CHIPSELECT, CLOCK, STANDBY, SLEEP, POWEROFF,
    output reg [15:0] DATAOUT
);
    reg [15:0] mem [0:16383];
    always @(posedge CLOCK)
        if (CHIPSELECT) begin
            if (WREN) mem[ADDRESS] <= DATAIN;
            else      DATAOUT <= mem[ADDRESS];
        end
endmodule

