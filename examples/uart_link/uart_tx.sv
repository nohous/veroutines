// A behavioral UART transmitter: 8N1, LSB first, one bit every DIV clocks.
// The line idles high; a start pulse (while not busy) loads data and shifts out
// start(0), d0..d7, stop(1). txd is the only wire crossing to the C++ receiver.
module uart_tx #(
    parameter int DIV = 8
)(
    input  logic       clk,
    input  logic       rst,
    input  logic [7:0] data,
    input  logic       start,
    output logic       busy,
    output logic       txd
);
    localparam int DW = (DIV <= 2) ? 1 : $clog2(DIV);

    logic          sending;
    logic [3:0]    bitcnt;
    logic [DW-1:0] divcnt;
    logic [9:0]    shifter;

    assign busy = sending;
    assign txd  = sending ? shifter[0] : 1'b1;

    always_ff @(posedge clk) begin
        if (rst) begin
            sending <= 1'b0;
            bitcnt  <= '0;
            divcnt  <= '0;
            shifter <= 10'h3FF;
        end else if (!sending) begin
            if (start) begin
                sending <= 1'b1;
                shifter <= {1'b1, data, 1'b0};   // stop, data[7:0], start
                bitcnt  <= '0;
                divcnt  <= '0;
            end
        end else if (divcnt == DW'(DIV - 1)) begin
            divcnt  <= '0;
            shifter <= {1'b1, shifter[9:1]};      // shift right, feed idle 1
            if (bitcnt == 4'd9) begin
                sending <= 1'b0;                  // start + 8 data + stop sent
            end else begin
                bitcnt <= bitcnt + 4'd1;
            end
        end else begin
            divcnt <= divcnt + 1'b1;
        end
    end
endmodule
