module mux4(input logic [1:0] select, input logic [3:0] data,
            output logic value);
  always_comb begin
    case (select)
      2'b00: value = data[0];
      2'b01: value = data[1];
      2'b10: value = data[2];
      default: value = data[3];
    endcase
  end
endmodule
