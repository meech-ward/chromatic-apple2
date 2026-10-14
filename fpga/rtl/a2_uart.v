// SPDX-License-Identifier: MIT
// Original, single-clock 8N1 UART. CLOCK_HZ / BAUD must be at least 8.
module a2_uart #(parameter CLOCK_HZ=33554432, parameter BAUD=115200) (
    input wire clk, input wire reset, input wire rx,
    output reg [7:0] rx_data, output reg rx_valid,
    input wire [7:0] tx_data, input wire tx_start,
    output wire tx, output reg tx_busy
);
    localparam PERIOD=(CLOCK_HZ+BAUD/2)/BAUD;
    reg [1:0] rx_sync=3;
    reg [15:0] rx_timer=0, tx_timer=0;
    reg [3:0] rx_bit=0, tx_left=0;
    reg [1:0] rx_state=0;
    reg [7:0] rx_shift=0;
    reg [9:0] tx_shift=10'h3ff;
    assign tx=tx_shift[0];
    always @(posedge clk) begin
        if (reset) begin
            rx_sync<=3; rx_state<=0; rx_valid<=0; rx_data<=0;
            rx_timer<=0; rx_bit<=0; rx_shift<=0;
        end else begin
            rx_sync<={rx_sync[0],rx};
            rx_valid<=0;
            case (rx_state)
                0: if (!rx_sync[1]) begin rx_timer<=PERIOD/2; rx_state<=1; end
                1: if (rx_timer!=0) rx_timer<=rx_timer-1'b1;
                   else if (rx_sync[1]) rx_state<=0;
                   else begin rx_timer<=PERIOD-1; rx_bit<=0; rx_state<=2; end
                2: if (rx_timer!=0) rx_timer<=rx_timer-1'b1;
                   else begin
                       rx_shift[rx_bit]<=rx_sync[1]; rx_timer<=PERIOD-1;
                       if (rx_bit==7) rx_state<=3; else rx_bit<=rx_bit+1'b1;
                   end
                3: if (rx_timer!=0) rx_timer<=rx_timer-1'b1;
                   else begin
                       if (rx_sync[1]) begin rx_data<=rx_shift; rx_valid<=1; end
                       rx_state<=0;
                   end
            endcase
        end
        if (reset) begin
            tx_busy<=0; tx_shift<=10'h3ff; tx_timer<=0; tx_left<=0;
        end else if (!tx_busy) begin
            if (tx_start) begin
                tx_shift<={1'b1,tx_data,1'b0}; tx_busy<=1;
                tx_timer<=PERIOD-1; tx_left<=10;
            end
        end else if (tx_timer!=0) tx_timer<=tx_timer-1'b1;
        else begin
            tx_shift<={1'b1,tx_shift[9:1]}; tx_timer<=PERIOD-1;
            if (tx_left==1) begin tx_busy<=0; tx_left<=0; end
            else tx_left<=tx_left-1'b1;
        end
    end
endmodule
