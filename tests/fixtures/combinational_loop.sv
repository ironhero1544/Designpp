module combinational_loop(input logic enable, output logic y);
  logic feedback;
  assign feedback = enable ? ~feedback : 1'b0;
  assign y = feedback;
endmodule
