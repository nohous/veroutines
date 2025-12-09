module axibox (
    input logic clk,
    input logic rst,

    input  logic      s_tvalid,
    output logic      s_tready,
    input  logic[7:0] s_tdata,

    output logic      m_tvalid,
    input  logic      m_tready,
    output logic[7:0] m_tdata,

    output logic event_out

);

logic pipe_en;
logic[3:0] cntr = 0;
logic event_signal;

assign pipe_en = !m_tvalid || m_tready;
assign s_tready = pipe_en;

assign event_out = event_signal;

always_ff @(posedge clk) begin: bitreverse

    if(!m_tvalid || m_tready) begin
        /*for (int i = 0; i < $bits(s_tdata); i++) 
            m_tdata[i] <= s_tdata[$bits(s_tdata) - i - 1];*/
        m_tdata <= s_tdata;

        m_tvalid <= s_tvalid;
        if (s_tvalid) begin 
            if (cntr == 3) begin
                $display("EEEE!");
                event_signal <= 1;
            end 
            cntr <= cntr + 1;
            $display("B!");
        end
    end

    if (rst) begin
        m_tvalid <= 0;
    end
    
end: bitreverse

always begin
    for (int i = 0; i < 16; i++) begin
        @(posedge clk);
        $display("%04t CLK: 1 %d", $time, i);
    end
    $finish;
end

always @(event_signal) begin
    $display("event_signal?");
end

/*always begin
    #10;
    m_tvalid <= 1;
    #20;
    wait(0);
end*/

    
endmodule