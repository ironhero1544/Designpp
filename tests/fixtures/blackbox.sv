(* blackbox *) module external_cell(input logic a, output logic y);
endmodule

module blackbox_top(input logic a, output logic y);
  external_cell instance(.a(a), .y(y));
endmodule
