module signed_vector(input logic signed [7:0] a, input logic signed [7:0] b,
                     output logic signed [8:0] sum,
                     output logic less_than);
  assign sum = a + b;
  assign less_than = a < b;
endmodule
