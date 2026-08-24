module register_controls(input logic clock, input logic reset_n,
                         input logic enable, input logic data,
                         output logic async_q, output logic enabled_q);
  always_ff @(posedge clock or negedge reset_n)
    if (!reset_n) async_q <= 1'b0;
    else async_q <= data;
  always_ff @(posedge clock)
    if (enable) enabled_q <= data;
endmodule
