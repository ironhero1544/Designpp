module multi_clock(input logic clock_a, input logic clock_b, input logic data,
                   output logic q_a, output logic q_b);
  always_ff @(posedge clock_a) q_a <= data;
  always_ff @(posedge clock_b) q_b <= data;
endmodule
