// SPDX-License-Identifier: MIT
// Compiler-only smoke test. This image is not a device firmware release.
module toolchain_smoke #(parameter integer INVERT = 0, COUNTER_BIT = 23)
    (input wire CLK_FPGA, output wire FPGA_LED_R);
    reg [23:0] counter = 24'd0;
    always @(posedge CLK_FPGA)
        counter <= counter + 24'd1;
    assign FPGA_LED_R = counter[COUNTER_BIT] ^ INVERT[0];
endmodule
