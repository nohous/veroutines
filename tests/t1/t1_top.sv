/**
 * @brief Tier-1 cosim interlock top: four small behavioral experiments.
 *
 * flop     - one flop plus a comb passthrough: pins the read-phase and
 *            race-with-edge semantics across the boundary.
 * echo     - a multi-cycle FSM slave for the C++ master: on a req toggle,
 *            counts down two cycles, then latches data and toggles ack
 *            (ack lands 4 posedges after the toggle is first sampled).
 * svmaster - a pure-delay initial-block stimulus: the SV side is the
 *            master and the C++ side reacts; exercises model_next_event
 *            negotiation with no cosim event at the wake time.
 * counters - one counter per clock domain, clk (10 ns) and clk2 (7 ns),
 *            coprime periods storm same-slot multi-event handling.
 */

module t1_top (
    input  logic       clk,
    input  logic       clk2,

    input  logic       f_d,
    output logic       f_q,
    output logic       f_comb,

    input  logic       e_req,
    input  logic [7:0] e_data,
    output logic       e_ack,
    output logic [7:0] e_echo,

    input  logic       m_ack,
    output logic       m_req,
    output logic [7:0] m_data,

    output logic [15:0] c1_count,
    output logic [15:0] c2_count
);

    // ---- flop --------------------------------------------------------------

    always_ff @(posedge clk) begin
        f_q <= f_d;
    end

    assign f_comb = f_d;

    // ---- echo FSM ------------------------------------------------------------

    logic       busy  = 1'b0;
    logic       req_q = 1'b0;
    logic [1:0] cnt   = 2'd0;

    always_ff @(posedge clk) begin
        if (!busy && (e_req != req_q)) begin
            busy  <= 1'b1;
            cnt   <= 2'd2;
            req_q <= e_req;
        end else if (busy) begin
            if (cnt == 2'd0) begin
                e_echo <= e_data;
                e_ack  <= ~e_ack;
                busy   <= 1'b0;
            end else begin
                cnt <= cnt - 2'd1;
            end
        end
    end

    // ---- svmaster ------------------------------------------------------------

    initial begin
        m_req  = 1'b0;
        m_data = 8'd0;
        #23ns;
        forever begin
            m_data = m_data + 8'd3;
            m_req  = ~m_req;
            #40ns;
        end
    end

    // ---- counters ------------------------------------------------------------

    always_ff @(posedge clk) begin
        c1_count <= c1_count + 16'd1;
    end

    always_ff @(posedge clk2) begin
        c2_count <= c2_count + 16'd1;
    end

    // No reset on purpose: this rig is verilator-only, and verilator's
    // two-state model starts every variable at 0.

endmodule : t1_top
