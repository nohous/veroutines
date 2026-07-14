// The RTL half of a combinational datapath whose other half lives in the C++
// master. Purely combinational, no clock:
//   rdata  = ROM[addr]                 (a comb read of a 16-entry table)
//   result = (rdata ^ mask) + addr     (combines rdata with a C++-supplied mask)
// mask is derived from rdata on the C++ side, so the settled value of result
// depends on a comb path that crosses the boundary twice:
//   addr -> rdata -> mask -> result. The cosim delta loop relaxes it to a
// fixpoint within one time slot.
module comb_slave (
    input  logic [7:0] addr,
    input  logic [7:0] mask,
    output logic [7:0] rdata,
    output logic [7:0] result
);
    always_comb begin
        case (addr[3:0])
            4'h0: rdata = 8'hA3; 4'h1: rdata = 8'h17; 4'h2: rdata = 8'hC5; 4'h3: rdata = 8'h2E;
            4'h4: rdata = 8'h5B; 4'h5: rdata = 8'h99; 4'h6: rdata = 8'h04; 4'h7: rdata = 8'hF0;
            4'h8: rdata = 8'h3C; 4'h9: rdata = 8'h81; 4'hA: rdata = 8'h6D; 4'hB: rdata = 8'hBE;
            4'hC: rdata = 8'h12; 4'hD: rdata = 8'h47; 4'hE: rdata = 8'hD8; 4'hF: rdata = 8'h7A;
            default: rdata = 8'h00;
        endcase
    end

    assign result = (rdata ^ mask) + addr;
endmodule
