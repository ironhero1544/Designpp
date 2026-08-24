module fsm(input logic clock, input logic reset_n, input logic start,
           output logic busy);
  typedef enum logic [1:0] {idle, run, finish} state_t;
  state_t state, next;
  always_comb begin
    next = state;
    case (state)
      idle: if (start) next = run;
      run: next = finish;
      finish: next = idle;
    endcase
  end
  always_ff @(posedge clock or negedge reset_n)
    if (!reset_n) state <= idle;
    else state <= next;
  assign busy = state != idle;
endmodule
