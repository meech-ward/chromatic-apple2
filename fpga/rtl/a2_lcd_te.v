// SPDX-License-Identifier: MIT
// Synchronize the panel's TE output and count rising edges after initialization.
module a2_lcd_te (
    input wire clk, input wire reset, input wire te_async,
    output wire level, output reg [15:0] edge_count=0
);
    (* async_reg = "true" *) reg [1:0] te_sync=0;
    reg previous=0;
    assign level=te_sync[1];
    always @(posedge clk) begin
        if (reset) begin
            te_sync<=0;
            previous<=0;
            edge_count<=0;
        end else begin
            te_sync<={te_sync[0],te_async};
            previous<=te_sync[1];
            if (te_sync[1] && !previous) edge_count<=edge_count+1'b1;
        end
    end
endmodule
