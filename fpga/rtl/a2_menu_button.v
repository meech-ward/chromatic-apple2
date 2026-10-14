// SPDX-License-Identifier: MIT
// Retain physical MENU presses while the MCU is busy. The caller synchronizes
// pressed into clk first; reads of press_sequence never acknowledge or clear it.
module a2_menu_button #(parameter CLOCK_HZ=33554432) (
    input wire clk,
    input wire reset,
    input wire pressed,
    output reg [7:0] press_sequence=0
);
    localparam integer DEBOUNCE_CYCLES=CLOCK_HZ>=100 ? CLOCK_HZ/100 : 1;
    localparam integer COUNT_WIDTH=DEBOUNCE_CYCLES>1 ? $clog2(DEBOUNCE_CYCLES) : 1;
    reg [COUNT_WIDTH-1:0] count=0;
    reg stable_pressed=0;

    always @(posedge clk) begin
        if (reset) begin
            count<=0;
            stable_pressed<=0;
            press_sequence<=0;
        end else if (pressed==stable_pressed) count<=0;
        else if (count==DEBOUNCE_CYCLES-1) begin
            count<=0;
            stable_pressed<=pressed;
            if (pressed) press_sequence<=press_sequence+1'b1;
        end else count<=count+1'b1;
    end
endmodule
