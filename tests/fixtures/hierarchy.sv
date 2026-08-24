module hierarchy_child(input logic a, input logic b, output logic y);
  assign y = a & b;
endmodule

module hierarchy(input logic a, input logic b, output logic y);
  hierarchy_child child(.a(a), .b(b), .y(y));
endmodule
