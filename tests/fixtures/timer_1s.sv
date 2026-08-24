`timescale 1ns / 1ps

module timer_1s (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        start,
    output reg         done
);
    // 50 MHz -> 1 second count (0 through 49,999,999).
    localparam CNT_MAX = 50_000_000 - 1;

    reg [25:0] cnt;
    reg        running;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cnt     <= 0;
            running <= 0;
            done    <= 0;
        end else begin
            if (start && !running) begin
                running <= 1;
                cnt     <= 0;
                done    <= 0;
            end else if (running) begin
                if (cnt == CNT_MAX) begin
                    cnt     <= 0;
                    running <= 0;
                    done    <= 1;
                end else begin
                    cnt  <= cnt + 1;
                    done <= 0;
                end
            end else begin
                done <= 0;
            end
        end
    end
endmodule
