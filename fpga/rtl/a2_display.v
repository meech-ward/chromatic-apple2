// SPDX-License-Identifier: MIT
// Original ST7785M RGB test-pattern generator. One clock domain, no vendor IP.
// A pixel uses three 6-bit RGB transfers. 912 * 158 clocks/frame at 8.388608MHz.
module a2_display #(
    parameter H_SYNC=30, H_BACK=35, H_ACTIVE=720, H_FRONT=127,
    parameter V_SYNC=2, V_BACK=2, V_ACTIVE=144, V_FRONT=10
) (
    input wire clk, input wire reset, input wire enabled,
    input wire frame_ready, input wire [3:0] pixel_data,
    output wire [14:0] pixel_address,
    output wire dotclk, output wire hsync, output wire vsync,
    output wire data_enable, output wire [5:0] data,
    output wire [9:0] scan_x, output wire [8:0] scan_y
);
    localparam H_TOTAL=H_SYNC+H_BACK+H_ACTIVE+H_FRONT;
    localparam V_TOTAL=V_SYNC+V_BACK+V_ACTIVE+V_FRONT;
    reg [1:0] divider=0, color_phase=0;
    reg [9:0] x=0;
    reg [8:0] y=0;
    reg [3:0] held_pixel=0;
    wire [9:0] next_x=x==H_TOTAL-1 ? 10'd0 : x+1'b1;
    wire [8:0] next_y=x==H_TOTAL-1 ? (y==V_TOTAL-1 ? 9'd0 : y+1'b1) : y;
    wire [9:0] next_dot=next_x-(H_SYNC+H_BACK);
    wire next_in_frame=next_x>=H_SYNC+H_BACK+120 && next_x<H_SYNC+H_BACK+600 &&
                       next_y>=V_SYNC+V_BACK && next_y<V_SYNC+V_BACK+144;
    // Prefetch one component ahead; the BRAM's one-cycle result is held at the
    // rising DOTCLK edge, leaving two whole clk periods before falling capture.
    assign pixel_address=next_in_frame ? (next_y-(V_SYNC+V_BACK))*160+next_dot/3-40 : 15'd0;
    assign scan_x=x;
    assign scan_y=y;
    assign dotclk=divider[1];
    assign hsync=x>=H_SYNC;
    assign vsync=y>=V_SYNC;
    assign data_enable=enabled && x>=H_SYNC+H_BACK && x<H_SYNC+H_BACK+H_ACTIVE &&
                       y>=V_SYNC+V_BACK && y<V_SYNC+V_BACK+V_ACTIVE;
    wire first_bar=x<H_SYNC+H_BACK+H_ACTIVE/3;
    wire second_bar=x<H_SYNC+H_BACK+2*H_ACTIVE/3;
    wire selected_color=color_phase==0 ? first_bar : color_phase==1 ? (!first_bar && second_bar) : !second_bar;
    wire in_frame=x>=H_SYNC+H_BACK+120 && x<H_SYNC+H_BACK+600;
    reg [17:0] rgb;
    always @* begin
        case(held_pixel)
            0: rgb={6'd0,6'd0,6'd0};
            1: rgb={6'd32,6'd0,6'd32};
            2: rgb={6'd0,6'd0,6'd42};
            3: rgb={6'd42,6'd0,6'd42};
            4: rgb={6'd0,6'd32,6'd0};
            5: rgb={6'd21,6'd21,6'd21};
            6: rgb={6'd0,6'd21,6'd63};
            7: rgb={6'd32,6'd32,6'd63};
            8: rgb={6'd32,6'd16,6'd0};
            9: rgb={6'd63,6'd21,6'd0};
            10: rgb={6'd42,6'd42,6'd42};
            11: rgb={6'd63,6'd32,6'd42};
            12: rgb={6'd0,6'd63,6'd0};
            13: rgb={6'd63,6'd63,6'd0};
            14: rgb={6'd0,6'd63,6'd63};
            default: rgb={6'd63,6'd63,6'd63};
        endcase
    end
    wire [5:0] component=color_phase==0 ? rgb[17:12] : color_phase==1 ? rgb[11:6] : rgb[5:0];
    assign data=!data_enable ? 6'd0 : frame_ready ? (in_frame ? component : 6'd0) : (selected_color ? 6'h3f : 6'd0);
    always @(posedge clk) begin
        if (reset) begin divider<=0; color_phase<=0; x<=0; y<=0; held_pixel<=0; end
        else begin
            divider<=divider+1'b1;
            // Board B1/C2 selects falling capture: launch on the opposite edge.
            if (divider==1) begin
                held_pixel<=pixel_data;
                if (x==H_TOTAL-1) begin
                    x<=0; color_phase<=0;
                    y<=y==V_TOTAL-1 ? 0 : y+1'b1;
                end else begin
                    x<=x+1'b1;
                    color_phase<=x==H_SYNC+H_BACK-1 ? 0 :
                                 color_phase==2 ? 0 : color_phase+1'b1;
                end
            end
        end
    end
endmodule
